#include "../src/ops_join.h"
#include "../src/ops_basic.h"
#include "../src/planner.h"
#include "../src/value.h"
#include "../src/group_key.h"

#include <iostream>
#include <cassert>
#include <vector>
#include <deque>
#include <random>
#include <algorithm>
#include <string>
#include <map>
#include <cmath>

// MockOperator yields a predefined list of tuples
class MockOperator : public Operator {
public:
    MockOperator(std::vector<Tuple> rows, std::vector<std::string> col_names = {})
        : rows_(std::move(rows)), col_names_(std::move(col_names)) {
        if (col_names_.empty()) {
            size_t w = rows_.empty() ? 1 : rows_[0].values.size();
            for (size_t i = 0; i < w; ++i) {
                col_names_.push_back("c" + std::to_string(i));
            }
        }
    }

    bool open() override {
        cursor_ = 0;
        open_called = true;
        return true;
    }

    bool next(Tuple& out) override {
        pull_count++;
        if (cursor_ >= rows_.size()) {
            return false;
        }
        out = rows_[cursor_++];
        return true;
    }

    void close() override {
        close_called = true;
    }

    const std::vector<std::string>& columnNames() const override {
        return col_names_;
    }

    bool open_called = false;
    bool close_called = false;
    size_t pull_count = 0;

private:
    std::vector<Tuple> rows_;
    std::vector<std::string> col_names_;
    size_t cursor_ = 0;
};

// Converts a tuple to a canonical string for multiset comparison
static std::string tupleToString(const Tuple& t) {
    std::string s;
    for (size_t i = 0; i < t.values.size(); ++i) {
        if (i > 0) s += "|";
        const auto& v = t.values[i];
        switch (v.type) {
            case V_NULL: s += "NULL"; break;
            case V_INT: s += "I:" + std::to_string(v.i); break;
            case V_FLOAT: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "F:%.6g", v.d);
                s += buf;
                break;
            }
            case V_STRING: s += "S:" + std::string(v.s); break;
        }
    }
    return s;
}

// -----------------------------------------------------------------------------
// 1. Affinity Normalization & SQLite Consistency Unit Tests
// -----------------------------------------------------------------------------
void testAffinityNormalization() {
    std::cout << "Testing SQLite Affinity Normalization...\n";

    char text_buf[64];

    // Case 1: INT 7 vs NUMERIC -> stays INT 7
    Value v1 = makeInt(7);
    Value n1 = normalizeKeyForAffinity(v1, Affinity::NUMERIC, Affinity::NUMERIC, text_buf, sizeof(text_buf));
    assert(n1.type == V_INT && n1.i == 7);

    // Case 2: TEXT "7" vs NUMERIC -> converts to INT 7
    Value v2 = makeString("7");
    Value n2 = normalizeKeyForAffinity(v2, Affinity::TEXT, Affinity::NUMERIC, text_buf, sizeof(text_buf));
    assert(n2.type == V_INT && n2.i == 7);

    // Case 3: TEXT "007" vs NUMERIC -> converts to INT 7
    Value v3 = makeString("007");
    Value n3 = normalizeKeyForAffinity(v3, Affinity::TEXT, Affinity::NUMERIC, text_buf, sizeof(text_buf));
    assert(n3.type == V_INT && n3.i == 7);

    // Case 4: TEXT " 7 " vs NUMERIC -> converts to INT 7
    Value v4 = makeString(" 7 ");
    Value n4 = normalizeKeyForAffinity(v4, Affinity::TEXT, Affinity::NUMERIC, text_buf, sizeof(text_buf));
    assert(n4.type == V_INT && n4.i == 7);

    // Case 5: TEXT "7.0" vs NUMERIC -> converts to FLOAT 7.0
    Value v5 = makeString("7.0");
    Value n5 = normalizeKeyForAffinity(v5, Affinity::TEXT, Affinity::NUMERIC, text_buf, sizeof(text_buf));
    assert(n5.type == V_FLOAT && n5.d == 7.0);

    // Case 6: TEXT "abc" vs NUMERIC -> non-numeric, stays TEXT "abc"
    Value v6 = makeString("abc");
    Value n6 = normalizeKeyForAffinity(v6, Affinity::TEXT, Affinity::NUMERIC, text_buf, sizeof(text_buf));
    assert(n6.type == V_STRING && n6.s == "abc");

    // Case 7: INT 7 vs TEXT (when own affinity is NONE) -> converts to TEXT "7"
    Value v7 = makeInt(7);
    Value n7 = normalizeKeyForAffinity(v7, Affinity::NONE, Affinity::TEXT, text_buf, sizeof(text_buf));
    assert(n7.type == V_STRING && n7.s == "7");

    // Case 8: encodeGroupKey consistency across normalized keys
    std::string key1, key2, key3, key4, key5, key6;
    encodeGroupKey(&n1, 1, key1);
    encodeGroupKey(&n2, 1, key2);
    encodeGroupKey(&n3, 1, key3);
    encodeGroupKey(&n4, 1, key4);
    encodeGroupKey(&n5, 1, key5);
    encodeGroupKey(&n6, 1, key6);

    assert(key1 == key2 && "INT 7 key equals normalized '7'");
    assert(key1 == key3 && "INT 7 key equals normalized '007'");
    assert(key1 == key4 && "INT 7 key equals normalized ' 7 '");
    assert(key1 == key5 && "INT 7 key equals normalized '7.0' (integral float equals int)");
    assert(key1 != key6 && "INT 7 key does not equal normalized 'abc'");

    std::cout << "  -> Affinity normalization passed!\n";
}

// -----------------------------------------------------------------------------
// 2. Zero Kept Build Columns Unit Test
// -----------------------------------------------------------------------------
void testZeroKeptBuildColumns() {
    std::cout << "Testing Zero Kept Build Columns (e.g. SELECT COUNT(*))...\n";

    // Probe: 5 rows (probe_id, probe_val)
    std::vector<Tuple> probe_rows;
    for (int i = 1; i <= 5; ++i) {
        probe_rows.push_back({{makeInt(i), makeString("p" + std::to_string(i))}});
    }

    // Build: 3 rows (build_id, build_val) matching ids 2, 3, 4
    std::vector<Tuple> build_rows;
    build_rows.push_back({{makeInt(2), makeString("b2")}});
    build_rows.push_back({{makeInt(3), makeString("b3")}});
    build_rows.push_back({{makeInt(4), makeString("b4")}});

    // Inner Join with 0 kept build columns
    {
        std::vector<std::unique_ptr<BoundExpr>> probe_keys;
        auto pk = std::make_unique<BoundExpr>();
        pk->kind = BoundExprKind::COLUMN_REF;
        pk->col_idx = 0;
        pk->affinity = Affinity::NUMERIC;
        probe_keys.push_back(std::move(pk));

        std::vector<std::unique_ptr<BoundExpr>> build_keys;
        auto bk = std::make_unique<BoundExpr>();
        bk->kind = BoundExprKind::COLUMN_REF;
        bk->col_idx = 0;
        bk->affinity = Affinity::NUMERIC;
        build_keys.push_back(std::move(bk));

        std::vector<size_t> empty_kept_slots; // zero kept build columns!

        HashJoinOperator join_op(
            std::make_unique<MockOperator>(probe_rows),
            std::make_unique<MockOperator>(build_rows),
            JoinType::INNER,
            /*swapped=*/false,
            std::move(probe_keys),
            std::move(build_keys),
            {Affinity::NUMERIC},
            {Affinity::NUMERIC},
            nullptr,
            /*probe_width=*/2,
            /*build_width=*/2,
            empty_kept_slots,
            /*written_left_w=*/2,
            /*written_right_w=*/2,
            /*is_nested_loop=*/false,
            /*join_stats=*/false
        );

        assert(join_op.open());
        Tuple out;
        size_t count = 0;
        while (join_op.next(out)) {
            count++;
            assert(out.values.size() == 4 && "Output tuple size matches total width");
            // probe columns present
            assert(out.values[0].type == V_INT);
            // build columns are V_NULL since kept_build_slots is empty
            assert(out.values[2].type == V_NULL);
            assert(out.values[3].type == V_NULL);
        }
        join_op.close();
        assert(count == 3 && "Exactly 3 matching rows output");
    }

    // Left Join with 0 kept build columns
    {
        std::vector<std::unique_ptr<BoundExpr>> probe_keys;
        auto pk = std::make_unique<BoundExpr>();
        pk->kind = BoundExprKind::COLUMN_REF;
        pk->col_idx = 0;
        pk->affinity = Affinity::NUMERIC;
        probe_keys.push_back(std::move(pk));

        std::vector<std::unique_ptr<BoundExpr>> build_keys;
        auto bk = std::make_unique<BoundExpr>();
        bk->kind = BoundExprKind::COLUMN_REF;
        bk->col_idx = 0;
        bk->affinity = Affinity::NUMERIC;
        build_keys.push_back(std::move(bk));

        std::vector<size_t> empty_kept_slots;

        HashJoinOperator join_op(
            std::make_unique<MockOperator>(probe_rows),
            std::make_unique<MockOperator>(build_rows),
            JoinType::LEFT,
            /*swapped=*/false,
            std::move(probe_keys),
            std::move(build_keys),
            {Affinity::NUMERIC},
            {Affinity::NUMERIC},
            nullptr,
            /*probe_width=*/2,
            /*build_width=*/2,
            empty_kept_slots,
            /*written_left_w=*/2,
            /*written_right_w=*/2,
            /*is_nested_loop=*/false,
            /*join_stats=*/false
        );

        assert(join_op.open());
        Tuple out;
        size_t count = 0;
        while (join_op.next(out)) {
            count++;
        }
        join_op.close();
        assert(count == 5 && "All 5 probe rows output for LEFT join with zero kept build columns");
    }

    std::cout << "  -> Zero kept build columns test passed!\n";
}

// -----------------------------------------------------------------------------
// 3. Duplicate Key Multiplication & Resumable Iteration
// -----------------------------------------------------------------------------
void testDuplicateKeys() {
    std::cout << "Testing Duplicate Key Multiplication...\n";

    // 5 probe rows with key = 100
    std::vector<Tuple> probe_rows;
    for (int i = 0; i < 5; ++i) {
        probe_rows.push_back({{makeInt(100), makeInt(i)}});
    }

    // 10 build rows with key = 100
    std::vector<Tuple> build_rows;
    for (int j = 0; j < 10; ++j) {
        build_rows.push_back({{makeInt(100), makeInt(j * 10)}});
    }

    std::vector<std::unique_ptr<BoundExpr>> probe_keys;
    auto pk = std::make_unique<BoundExpr>();
    pk->kind = BoundExprKind::COLUMN_REF;
    pk->col_idx = 0;
    pk->affinity = Affinity::NUMERIC;
    probe_keys.push_back(std::move(pk));

    std::vector<std::unique_ptr<BoundExpr>> build_keys;
    auto bk = std::make_unique<BoundExpr>();
    bk->kind = BoundExprKind::COLUMN_REF;
    bk->col_idx = 0;
    bk->affinity = Affinity::NUMERIC;
    build_keys.push_back(std::move(bk));

    std::vector<size_t> kept = {0, 1};

    HashJoinOperator join_op(
        std::make_unique<MockOperator>(probe_rows),
        std::make_unique<MockOperator>(build_rows),
        JoinType::INNER,
        /*swapped=*/false,
        std::move(probe_keys),
        std::move(build_keys),
        {Affinity::NUMERIC},
        {Affinity::NUMERIC},
        nullptr,
        2, 2, kept, 2, 2, false, false
    );

    assert(join_op.open());
    Tuple out;
    size_t count = 0;
    while (join_op.next(out)) {
        count++;
        assert(out.values[0].i == 100);
        assert(out.values[2].i == 100);
    }
    join_op.close();

    assert(count == 50 && "5 probe rows * 10 build rows = 50 output rows");
    std::cout << "  -> Duplicate key multiplication passed (50 rows)!\n";
}

// -----------------------------------------------------------------------------
// 4. Pushdown Legality Truth Table
// -----------------------------------------------------------------------------
void testPushdownLegality() {
    std::cout << "Testing Pushdown Legality Truth Table...\n";

    // Example 1: a RIGHT JOIN b -> a: no, b: yes
    {
        std::vector<JoinClause> joins(1);
        joins[0].type = JoinType::RIGHT;
        assert(isTablePushdownLegal(0, joins) == false);
        assert(isTablePushdownLegal(1, joins) == true);
    }

    // Example 2: a JOIN b RIGHT JOIN c -> a: no, b: no, c: yes
    {
        std::vector<JoinClause> joins(2);
        joins[0].type = JoinType::INNER;
        joins[1].type = JoinType::RIGHT;
        assert(isTablePushdownLegal(0, joins) == false);
        assert(isTablePushdownLegal(1, joins) == false);
        assert(isTablePushdownLegal(2, joins) == true);
    }

    // Example 3: a LEFT JOIN b RIGHT JOIN c -> a: no, b: no, c: yes
    {
        std::vector<JoinClause> joins(2);
        joins[0].type = JoinType::LEFT;
        joins[1].type = JoinType::RIGHT;
        assert(isTablePushdownLegal(0, joins) == false);
        assert(isTablePushdownLegal(1, joins) == false);
        assert(isTablePushdownLegal(2, joins) == true);
    }

    // Example 4: a RIGHT JOIN b JOIN c -> a: no, b: yes, c: yes
    {
        std::vector<JoinClause> joins(2);
        joins[0].type = JoinType::RIGHT;
        joins[1].type = JoinType::INNER;
        assert(isTablePushdownLegal(0, joins) == false);
        assert(isTablePushdownLegal(1, joins) == true);
        assert(isTablePushdownLegal(2, joins) == true);
    }

    // Example 5: a FULL JOIN b -> a: no, b: no
    {
        std::vector<JoinClause> joins(1);
        joins[0].type = JoinType::FULL;
        assert(isTablePushdownLegal(0, joins) == false);
        assert(isTablePushdownLegal(1, joins) == false);
    }

    // Example 6: a LEFT JOIN b -> a: yes, b: no
    {
        std::vector<JoinClause> joins(1);
        joins[0].type = JoinType::LEFT;
        assert(isTablePushdownLegal(0, joins) == true);
        assert(isTablePushdownLegal(1, joins) == false);
    }

    // Example 7: a JOIN b JOIN c -> a: yes, b: yes, c: yes
    {
        std::vector<JoinClause> joins(2);
        joins[0].type = JoinType::INNER;
        joins[1].type = JoinType::INNER;
        assert(isTablePushdownLegal(0, joins) == true);
        assert(isTablePushdownLegal(1, joins) == true);
        assert(isTablePushdownLegal(2, joins) == true);
    }

    std::cout << "  -> Pushdown legality truth table passed!\n";
}

// -----------------------------------------------------------------------------
// 5. Residual Rejection and Null-Extension Tests (Change 3)
// -----------------------------------------------------------------------------
void testResidualRejectionNullExtended() {
    std::cout << "Testing Residual Rejection and NULL-Extension (Hash & Nested-Loop)...\n";

    std::vector<Tuple> probe_rows = {{{makeInt(1), makeInt(10)}}};
    std::vector<Tuple> build_rows = {{{makeInt(1), makeInt(20)}}};

    // Subtest 1: Hash mode RIGHT join - keys match (1 == 1) but residual (probe.val > build.val) rejects candidate
    {
        std::vector<std::unique_ptr<BoundExpr>> p_keys;
        auto pk = std::make_unique<BoundExpr>();
        pk->kind = BoundExprKind::COLUMN_REF;
        pk->col_idx = 0;
        pk->affinity = Affinity::NUMERIC;
        p_keys.push_back(std::move(pk));

        std::vector<std::unique_ptr<BoundExpr>> b_keys;
        auto bk = std::make_unique<BoundExpr>();
        bk->kind = BoundExprKind::COLUMN_REF;
        bk->col_idx = 0;
        bk->affinity = Affinity::NUMERIC;
        b_keys.push_back(std::move(bk));

        // Residual: probe.val > build.val (10 > 20 -> false)
        auto residual = std::make_unique<BoundExpr>();
        residual->kind = BoundExprKind::BINARY_OP;
        residual->op = ">";
        auto l = std::make_unique<BoundExpr>();
        l->kind = BoundExprKind::COLUMN_REF;
        l->col_idx = 1; // probe.val
        auto r = std::make_unique<BoundExpr>();
        r->kind = BoundExprKind::COLUMN_REF;
        r->col_idx = 3; // build.val (probe_w = 2, 2 + 1 = 3)
        residual->left = std::move(l);
        residual->right = std::move(r);

        std::vector<size_t> kept = {0, 1};

        HashJoinOperator r_join(
            std::make_unique<MockOperator>(probe_rows),
            std::make_unique<MockOperator>(build_rows),
            JoinType::RIGHT,
            /*swapped=*/false,
            /*emit_unmatched_probe=*/false,
            /*emit_unmatched_build=*/true,
            std::move(p_keys),
            std::move(b_keys),
            {Affinity::NUMERIC},
            {Affinity::NUMERIC},
            std::move(residual),
            2, 2, kept, 2, 2,
            /*is_nested_loop=*/false,
            false
        );

        assert(r_join.open());
        Tuple out;
        size_t count = 0;
        while (r_join.next(out)) {
            count++;
            // Build row must be emitted once NULL-extended on probe slots
            assert(out.values[0].type == V_NULL);
            assert(out.values[1].type == V_NULL);
            assert(out.values[2].i == 1);
            assert(out.values[3].i == 20);
        }
        r_join.close();
        assert(count == 1 && "RIGHT join must emit unmatched build row");
    }

    // Subtest 2: Hash mode FULL join - keys match, residual rejects candidate
    {
        std::vector<std::unique_ptr<BoundExpr>> p_keys;
        auto pk = std::make_unique<BoundExpr>();
        pk->kind = BoundExprKind::COLUMN_REF;
        pk->col_idx = 0;
        pk->affinity = Affinity::NUMERIC;
        p_keys.push_back(std::move(pk));

        std::vector<std::unique_ptr<BoundExpr>> b_keys;
        auto bk = std::make_unique<BoundExpr>();
        bk->kind = BoundExprKind::COLUMN_REF;
        bk->col_idx = 0;
        bk->affinity = Affinity::NUMERIC;
        b_keys.push_back(std::move(bk));

        auto residual = std::make_unique<BoundExpr>();
        residual->kind = BoundExprKind::BINARY_OP;
        residual->op = ">";
        auto l = std::make_unique<BoundExpr>();
        l->kind = BoundExprKind::COLUMN_REF;
        l->col_idx = 1;
        auto r = std::make_unique<BoundExpr>();
        r->kind = BoundExprKind::COLUMN_REF;
        r->col_idx = 3;
        residual->left = std::move(l);
        residual->right = std::move(r);

        std::vector<size_t> kept = {0, 1};

        HashJoinOperator f_join(
            std::make_unique<MockOperator>(probe_rows),
            std::make_unique<MockOperator>(build_rows),
            JoinType::FULL,
            /*swapped=*/false,
            /*emit_unmatched_probe=*/true,
            /*emit_unmatched_build=*/true,
            std::move(p_keys),
            std::move(b_keys),
            {Affinity::NUMERIC},
            {Affinity::NUMERIC},
            std::move(residual),
            2, 2, kept, 2, 2,
            /*is_nested_loop=*/false,
            false
        );

        assert(f_join.open());
        Tuple out;
        std::vector<std::string> rows;
        while (f_join.next(out)) {
            rows.push_back(tupleToString(out));
        }
        f_join.close();
        assert(rows.size() == 2 && "FULL join must emit both probe and build NULL-extended");
        assert(rows[0] == "I:1|I:10|NULL|NULL");
        assert(rows[1] == "NULL|NULL|I:1|I:20");
    }

    // Subtest 3: Nested-loop mode RIGHT join - residual rejects candidate
    {
        auto residual = std::make_unique<BoundExpr>();
        residual->kind = BoundExprKind::BINARY_OP;
        residual->op = ">";
        auto l = std::make_unique<BoundExpr>();
        l->kind = BoundExprKind::COLUMN_REF;
        l->col_idx = 1;
        auto r = std::make_unique<BoundExpr>();
        r->kind = BoundExprKind::COLUMN_REF;
        r->col_idx = 3;
        residual->left = std::move(l);
        residual->right = std::move(r);

        std::vector<size_t> kept = {0, 1};

        HashJoinOperator r_nl_join(
            std::make_unique<MockOperator>(probe_rows),
            std::make_unique<MockOperator>(build_rows),
            JoinType::RIGHT,
            /*swapped=*/false,
            /*emit_unmatched_probe=*/false,
            /*emit_unmatched_build=*/true,
            {}, {}, {}, {},
            std::move(residual),
            2, 2, kept, 2, 2,
            /*is_nested_loop=*/true,
            false
        );

        assert(r_nl_join.open());
        Tuple out;
        size_t count = 0;
        while (r_nl_join.next(out)) {
            count++;
            assert(out.values[0].type == V_NULL);
            assert(out.values[1].type == V_NULL);
            assert(out.values[2].i == 1);
            assert(out.values[3].i == 20);
        }
        r_nl_join.close();
        assert(count == 1);
    }

    // Subtest 4: Nested-loop mode FULL join - residual rejects candidate
    {
        auto residual = std::make_unique<BoundExpr>();
        residual->kind = BoundExprKind::BINARY_OP;
        residual->op = ">";
        auto l = std::make_unique<BoundExpr>();
        l->kind = BoundExprKind::COLUMN_REF;
        l->col_idx = 1;
        auto r = std::make_unique<BoundExpr>();
        r->kind = BoundExprKind::COLUMN_REF;
        r->col_idx = 3;
        residual->left = std::move(l);
        residual->right = std::move(r);

        std::vector<size_t> kept = {0, 1};

        HashJoinOperator f_nl_join(
            std::make_unique<MockOperator>(probe_rows),
            std::make_unique<MockOperator>(build_rows),
            JoinType::FULL,
            /*swapped=*/false,
            /*emit_unmatched_probe=*/true,
            /*emit_unmatched_build=*/true,
            {}, {}, {}, {},
            std::move(residual),
            2, 2, kept, 2, 2,
            /*is_nested_loop=*/true,
            false
        );

        assert(f_nl_join.open());
        Tuple out;
        std::vector<std::string> rows;
        while (f_nl_join.next(out)) {
            rows.push_back(tupleToString(out));
        }
        f_nl_join.close();
        assert(rows.size() == 2);
        assert(rows[0] == "I:1|I:10|NULL|NULL");
        assert(rows[1] == "NULL|NULL|I:1|I:20");
    }

    std::cout << "  -> Residual rejection tests passed!\n";
}

// -----------------------------------------------------------------------------
// 6. Swapped RIGHT Join with NULL Key in Probe (Change user note)
// -----------------------------------------------------------------------------
void testSwappedRightProbeNullKey() {
    std::cout << "Testing Swapped RIGHT Join with NULL Key in Probe (R)...\n";

    // Customers (L, build): id=1, name="Alice"
    std::vector<Tuple> cust_rows = {{{makeInt(1), makeString("Alice")}}};

    // Orders (R, probe): cust_id=NULL, amount=99.5
    std::vector<Tuple> order_rows = {{{makeNull(), makeFloat(99.5)}}};

    std::vector<std::unique_ptr<BoundExpr>> probe_keys;
    auto pk = std::make_unique<BoundExpr>();
    pk->kind = BoundExprKind::COLUMN_REF;
    pk->col_idx = 0; // order.cust_id
    pk->affinity = Affinity::NUMERIC;
    probe_keys.push_back(std::move(pk));

    std::vector<std::unique_ptr<BoundExpr>> build_keys;
    auto bk = std::make_unique<BoundExpr>();
    bk->kind = BoundExprKind::COLUMN_REF;
    bk->col_idx = 0; // customer.id
    bk->affinity = Affinity::NUMERIC;
    build_keys.push_back(std::move(bk));

    std::vector<size_t> kept = {0, 1};

    // Swapped RIGHT: probe=orders, build=customers, swapped=true, emit_unmatched_probe=true, emit_unmatched_build=false
    HashJoinOperator swapped_right(
        std::make_unique<MockOperator>(order_rows),   // probe = orders (R)
        std::make_unique<MockOperator>(cust_rows),    // build = customers (L)
        JoinType::RIGHT,
        /*swapped=*/true,
        /*emit_unmatched_probe=*/true,
        /*emit_unmatched_build=*/false,
        std::move(probe_keys),
        std::move(build_keys),
        {Affinity::NUMERIC},
        {Affinity::NUMERIC},
        nullptr,
        2, 2, kept, 2, 2,
        false, false
    );

    assert(swapped_right.open());
    Tuple out;
    size_t count = 0;
    while (swapped_right.next(out)) {
        count++;
        // Must appear exactly once, NULL-extended on the left (customers slots = NULL)
        assert(out.values[0].type == V_NULL);
        assert(out.values[1].type == V_NULL);
        assert(out.values[2].type == V_NULL);
        assert(out.values[3].type == V_FLOAT && std::abs(out.values[3].d - 99.5) < 1e-6);
    }
    swapped_right.close();
    assert(count == 1 && "Swapped RIGHT must emit NULL-key probe row exactly once");

    std::cout << "  -> Swapped RIGHT probe NULL key passed!\n";
}

// -----------------------------------------------------------------------------
// 7. NULL Key Build Row Storage and Emission
// -----------------------------------------------------------------------------
void testNullKeyStorageAndEmission() {
    std::cout << "Testing NULL Key Storage and Emission (INNER/LEFT vs RIGHT/FULL)...\n";

    std::vector<Tuple> probe_rows = {{{makeInt(1), makeString("p1")}}};
    std::vector<Tuple> build_rows = {
        {{makeNull(), makeString("b_null")}},
        {{makeInt(1), makeString("b1")}}
    };

    auto make_keys = []() {
        std::vector<std::unique_ptr<BoundExpr>> keys;
        auto k = std::make_unique<BoundExpr>();
        k->kind = BoundExprKind::COLUMN_REF;
        k->col_idx = 0;
        k->affinity = Affinity::NUMERIC;
        keys.push_back(std::move(k));
        return keys;
    };

    std::vector<size_t> kept = {0, 1};

    // INNER: NULL-key row NOT stored
    {
        HashJoinOperator inner_op(
            std::make_unique<MockOperator>(probe_rows),
            std::make_unique<MockOperator>(build_rows),
            JoinType::INNER, false, false, false,
            make_keys(), make_keys(), {Affinity::NUMERIC}, {Affinity::NUMERIC},
            nullptr, 2, 2, kept, 2, 2, false, false
        );
        assert(inner_op.open());
        assert(inner_op.buildRowCount() == 1 && "INNER must NOT store NULL-key build rows");
        inner_op.close();
    }

    // LEFT: NULL-key row NOT stored
    {
        HashJoinOperator left_op(
            std::make_unique<MockOperator>(probe_rows),
            std::make_unique<MockOperator>(build_rows),
            JoinType::LEFT, false, true, false,
            make_keys(), make_keys(), {Affinity::NUMERIC}, {Affinity::NUMERIC},
            nullptr, 2, 2, kept, 2, 2, false, false
        );
        assert(left_op.open());
        assert(left_op.buildRowCount() == 1 && "LEFT must NOT store NULL-key build rows");
        left_op.close();
    }

    // RIGHT: NULL-key row IS stored and emitted once
    {
        HashJoinOperator right_op(
            std::make_unique<MockOperator>(probe_rows),
            std::make_unique<MockOperator>(build_rows),
            JoinType::RIGHT, false, false, true,
            make_keys(), make_keys(), {Affinity::NUMERIC}, {Affinity::NUMERIC},
            nullptr, 2, 2, kept, 2, 2, false, false
        );
        assert(right_op.open());
        assert(right_op.buildRowCount() == 2 && "RIGHT must store NULL-key build rows");
        Tuple out;
        std::vector<std::string> rows;
        while (right_op.next(out)) {
            rows.push_back(tupleToString(out));
        }
        right_op.close();
        assert(rows.size() == 2);
        assert(rows[0] == "I:1|S:p1|I:1|S:b1");
        assert(rows[1] == "NULL|NULL|NULL|S:b_null");
    }

    // FULL: NULL-key row IS stored and emitted once
    {
        HashJoinOperator full_op(
            std::make_unique<MockOperator>(probe_rows),
            std::make_unique<MockOperator>(build_rows),
            JoinType::FULL, false, true, true,
            make_keys(), make_keys(), {Affinity::NUMERIC}, {Affinity::NUMERIC},
            nullptr, 2, 2, kept, 2, 2, false, false
        );
        assert(full_op.open());
        assert(full_op.buildRowCount() == 2 && "FULL must store NULL-key build rows");
        Tuple out;
        std::vector<std::string> rows;
        while (full_op.next(out)) {
            rows.push_back(tupleToString(out));
        }
        full_op.close();
        assert(rows.size() == 2);
        assert(rows[0] == "I:1|S:p1|I:1|S:b1");
        assert(rows[1] == "NULL|NULL|NULL|S:b_null");
    }

    std::cout << "  -> NULL key storage and emission tests passed!\n";
}

// -----------------------------------------------------------------------------
// 8. Empty Probe and Build Tables
// -----------------------------------------------------------------------------
void testEmptyProbeAndBuild() {
    std::cout << "Testing Empty Probe and Build Tables (RIGHT & FULL)...\n";

    std::vector<Tuple> empty_rows;
    std::vector<Tuple> non_empty_rows = {
        {{makeInt(10), makeString("v10")}},
        {{makeInt(20), makeString("v20")}}
    };

    auto make_keys = []() {
        std::vector<std::unique_ptr<BoundExpr>> keys;
        auto k = std::make_unique<BoundExpr>();
        k->kind = BoundExprKind::COLUMN_REF;
        k->col_idx = 0;
        k->affinity = Affinity::NUMERIC;
        keys.push_back(std::move(k));
        return keys;
    };

    std::vector<size_t> kept = {0, 1};

    // Subtest 1: Empty probe, non-empty build (RIGHT join -> emits all build rows NULL-extended on left)
    {
        HashJoinOperator r_op(
            std::make_unique<MockOperator>(empty_rows),
            std::make_unique<MockOperator>(non_empty_rows),
            JoinType::RIGHT, false, false, true,
            make_keys(), make_keys(), {Affinity::NUMERIC}, {Affinity::NUMERIC},
            nullptr, 2, 2, kept, 2, 2, false, false
        );
        assert(r_op.open());
        Tuple out;
        std::vector<std::string> rows;
        while (r_op.next(out)) {
            rows.push_back(tupleToString(out));
        }
        r_op.close();
        assert(rows.size() == 2);
        assert(rows[0] == "NULL|NULL|I:10|S:v10");
        assert(rows[1] == "NULL|NULL|I:20|S:v20");
    }

    // Subtest 2: Non-empty probe, empty build (FULL join -> emits all probe rows NULL-extended on right)
    {
        HashJoinOperator f_op(
            std::make_unique<MockOperator>(non_empty_rows),
            std::make_unique<MockOperator>(empty_rows),
            JoinType::FULL, false, true, true,
            make_keys(), make_keys(), {Affinity::NUMERIC}, {Affinity::NUMERIC},
            nullptr, 2, 2, kept, 2, 2, false, false
        );
        assert(f_op.open());
        Tuple out;
        std::vector<std::string> rows;
        while (f_op.next(out)) {
            rows.push_back(tupleToString(out));
        }
        f_op.close();
        assert(rows.size() == 2);
        assert(rows[0] == "I:10|S:v10|NULL|NULL");
        assert(rows[1] == "I:20|S:v20|NULL|NULL");
    }

    // Subtest 3: Non-empty probe, empty build (RIGHT join -> emits 0 rows)
    {
        HashJoinOperator r_op(
            std::make_unique<MockOperator>(non_empty_rows),
            std::make_unique<MockOperator>(empty_rows),
            JoinType::RIGHT, false, false, true,
            make_keys(), make_keys(), {Affinity::NUMERIC}, {Affinity::NUMERIC},
            nullptr, 2, 2, kept, 2, 2, false, false
        );
        assert(r_op.open());
        Tuple out;
        size_t count = 0;
        while (r_op.next(out)) count++;
        r_op.close();
        assert(count == 0);
    }

    std::cout << "  -> Empty probe/build tests passed!\n";
}

// -----------------------------------------------------------------------------
// 9. 1000 Randomized Property Tests vs Reference In-Test Join
// -----------------------------------------------------------------------------
void test1000RandomizedPropertyJoins() {
    std::cout << "Running 1000 Randomized Property Tests (INNER, LEFT, RIGHT, FULL vs Reference Join)...\n";

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> probe_len_dist(0, 30);
    std::uniform_int_distribution<size_t> build_len_dist(0, 30);
    std::uniform_int_distribution<int> key_dist(1, 10);
    std::uniform_int_distribution<int> null_dist(0, 4); // ~20% NULL keys
    std::uniform_int_distribution<int> join_kind_dist(0, 3); // 0=INNER, 1=LEFT, 2=RIGHT, 3=FULL
    std::uniform_int_distribution<int> nl_dist(0, 4); // 0 means nested loop (20% of cases)
    std::uniform_int_distribution<int> swap_dist(0, 1);
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_int_distribution<int> type_dist(0, 2); // 0=INT, 1=FLOAT, 2=STRING

    for (int iter = 0; iter < 1000; ++iter) {
        size_t p_len = probe_len_dist(rng);
        size_t b_len = build_len_dist(rng);
        int join_kind = join_kind_dist(rng); // 0=INNER, 1=LEFT, 2=RIGHT, 3=FULL
        bool is_nested_loop = (nl_dist(rng) == 0);
        bool has_residual = (bool_dist(rng) == 1);
        int key_type = type_dist(rng);

        JoinType jtype;
        bool emit_unmatched_probe = false;
        bool emit_unmatched_build = false;
        bool swapped = false;

        if (join_kind == 0) {
            jtype = JoinType::INNER;
            emit_unmatched_probe = false;
            emit_unmatched_build = false;
            swapped = (swap_dist(rng) == 1);
        } else if (join_kind == 1) {
            jtype = JoinType::LEFT;
            emit_unmatched_probe = true;
            emit_unmatched_build = false;
            swapped = false;
        } else if (join_kind == 2) {
            jtype = JoinType::RIGHT;
            if (swap_dist(rng) == 1) {
                // Swapped RIGHT: probe child is R (probe_tuples), build child is L (build_tuples), swapped layout
                emit_unmatched_probe = true;
                emit_unmatched_build = false;
                swapped = true;
            } else {
                emit_unmatched_probe = false;
                emit_unmatched_build = true;
                swapped = false;
            }
        } else {
            jtype = JoinType::FULL;
            emit_unmatched_probe = true;
            emit_unmatched_build = true;
            swapped = false;
        }

        std::deque<std::string> p_strings;
        std::vector<Tuple> probe_tuples;
        for (size_t i = 0; i < p_len; ++i) {
            Value k;
            if (null_dist(rng) == 0) {
                k = makeNull();
            } else {
                int kval = key_dist(rng);
                if (key_type == 0) k = makeInt(kval);
                else if (key_type == 1) k = makeFloat(static_cast<double>(kval));
                else {
                    p_strings.push_back("str_" + std::to_string(kval));
                    k = makeString(p_strings.back());
                }
            }
            Value payload = makeInt(static_cast<long long>(i));
            probe_tuples.push_back({{k, payload}});
        }

        std::deque<std::string> b_strings;
        std::vector<Tuple> build_tuples;
        for (size_t j = 0; j < b_len; ++j) {
            Value k;
            if (null_dist(rng) == 0) {
                k = makeNull();
            } else {
                int kval = key_dist(rng);
                if (key_type == 0) k = makeInt(kval);
                else if (key_type == 1) k = makeFloat(static_cast<double>(kval));
                else {
                    b_strings.push_back("str_" + std::to_string(kval));
                    k = makeString(b_strings.back());
                }
            }
            Value payload = makeInt(static_cast<long long>(j * 10));
            build_tuples.push_back({{k, payload}});
        }

        // Reference Join Output (Multiset)
        std::map<std::string, int> ref_multiset;
        std::vector<uint8_t> ref_probe_matched(p_len, 0);
        std::vector<uint8_t> ref_build_matched(b_len, 0);

        for (size_t p = 0; p < p_len; ++p) {
            const auto& pt = probe_tuples[p];
            for (size_t b = 0; b < b_len; ++b) {
                const auto& bt = build_tuples[b];

                bool keys_equal = false;
                if (is_nested_loop) {
                    keys_equal = true; // no equi-keys
                } else if (pt.values[0].type != V_NULL && bt.values[0].type != V_NULL) {
                    if (key_type == 0) keys_equal = (pt.values[0].i == bt.values[0].i);
                    else if (key_type == 1) keys_equal = (pt.values[0].d == bt.values[0].d);
                    else keys_equal = (pt.values[0].s == bt.values[0].s);
                }

                if (keys_equal) {
                    bool res_pass = true;
                    if (has_residual) {
                        // Residual: probe.payload <= build.payload
                        res_pass = (pt.values[1].i <= bt.values[1].i);
                    }
                    if (res_pass) {
                        ref_probe_matched[p] = 1;
                        ref_build_matched[b] = 1;
                        Tuple combined;
                        if (!swapped) {
                            combined.values = {pt.values[0], pt.values[1], bt.values[0], bt.values[1]};
                        } else {
                            combined.values = {bt.values[0], bt.values[1], pt.values[0], pt.values[1]};
                        }
                        ref_multiset[tupleToString(combined)]++;
                    }
                }
            }
        }

        if (emit_unmatched_probe) {
            for (size_t p = 0; p < p_len; ++p) {
                if (!ref_probe_matched[p]) {
                    Tuple combined;
                    if (!swapped) {
                        combined.values = {probe_tuples[p].values[0], probe_tuples[p].values[1], makeNull(), makeNull()};
                    } else {
                        combined.values = {makeNull(), makeNull(), probe_tuples[p].values[0], probe_tuples[p].values[1]};
                    }
                    ref_multiset[tupleToString(combined)]++;
                }
            }
        }

        if (emit_unmatched_build) {
            for (size_t b = 0; b < b_len; ++b) {
                if (!ref_build_matched[b]) {
                    Tuple combined;
                    if (!swapped) {
                        combined.values = {makeNull(), makeNull(), build_tuples[b].values[0], build_tuples[b].values[1]};
                    } else {
                        combined.values = {build_tuples[b].values[0], build_tuples[b].values[1], makeNull(), makeNull()};
                    }
                    ref_multiset[tupleToString(combined)]++;
                }
            }
        }

        // Run HashJoinOperator
        std::vector<std::unique_ptr<BoundExpr>> p_keys;
        std::vector<std::unique_ptr<BoundExpr>> b_keys;
        if (!is_nested_loop) {
            auto pk = std::make_unique<BoundExpr>();
            pk->kind = BoundExprKind::COLUMN_REF;
            pk->col_idx = 0;
            pk->affinity = (key_type == 2 ? Affinity::TEXT : Affinity::NUMERIC);
            p_keys.push_back(std::move(pk));

            auto bk = std::make_unique<BoundExpr>();
            bk->kind = BoundExprKind::COLUMN_REF;
            bk->col_idx = 0;
            bk->affinity = (key_type == 2 ? Affinity::TEXT : Affinity::NUMERIC);
            b_keys.push_back(std::move(bk));
        }

        std::unique_ptr<BoundExpr> residual = nullptr;
        if (has_residual) {
            residual = std::make_unique<BoundExpr>();
            residual->kind = BoundExprKind::BINARY_OP;
            residual->op = "<=";
            auto l = std::make_unique<BoundExpr>();
            l->kind = BoundExprKind::COLUMN_REF;
            auto r = std::make_unique<BoundExpr>();
            r->kind = BoundExprKind::COLUMN_REF;
            if (!swapped) {
                l->col_idx = 1; // probe.payload
                r->col_idx = 3; // build.payload (probe_w=2 + 1)
            } else {
                r->col_idx = 1; // build.payload
                l->col_idx = 3; // probe.payload (build_w=2 + 1)
            }
            residual->left = std::move(l);
            residual->right = std::move(r);
        }

        std::vector<size_t> kept = {0, 1};

        HashJoinOperator join_op(
            std::make_unique<MockOperator>(probe_tuples),
            std::make_unique<MockOperator>(build_tuples),
            jtype,
            swapped,
            emit_unmatched_probe,
            emit_unmatched_build,
            std::move(p_keys),
            std::move(b_keys),
            {key_type == 2 ? Affinity::TEXT : Affinity::NUMERIC},
            {key_type == 2 ? Affinity::TEXT : Affinity::NUMERIC},
            std::move(residual),
            2, 2, kept, 2, 2,
            is_nested_loop,
            false
        );

        assert(join_op.open());
        std::map<std::string, int> actual_multiset;
        Tuple out;
        while (join_op.next(out)) {
            actual_multiset[tupleToString(out)]++;
        }
        join_op.close();

        if (actual_multiset != ref_multiset) {
            std::cerr << "Mismatch at iter " << iter << " (p_len=" << p_len << ", b_len=" << b_len
                      << ", jtype=" << static_cast<int>(jtype) << ", swapped=" << swapped
                      << ", is_nl=" << is_nested_loop << ", has_residual=" << has_residual << ", key_type=" << key_type << ")\n";
            std::cerr << "Reference multiset (" << ref_multiset.size() << " entries):\n";
            for (const auto& [k, v] : ref_multiset) {
                std::cerr << "  ref: " << k << " count=" << v << "\n";
            }
            std::cerr << "Actual multiset (" << actual_multiset.size() << " entries):\n";
            for (const auto& [k, v] : actual_multiset) {
                std::cerr << "  act: " << k << " count=" << v << "\n";
            }
            assert(false && "HashJoinOperator output matches reference join multiset");
        }
    }

    std::cout << "  -> 1000 randomized property tests PASSED!\n";
}

// -----------------------------------------------------------------------------
// 6. Swapped Inner Join and Nested Loop Fallback
// -----------------------------------------------------------------------------
void testSwappedAndNestedLoop() {
    std::cout << "Testing Swapped Inner Join & Nested Loop Fallback...\n";

    // Table A: 3 rows: id=1, 2, 3
    std::vector<Tuple> rows_a = {
        {{makeInt(1), makeString("a1")}},
        {{makeInt(2), makeString("a2")}},
        {{makeInt(3), makeString("a3")}}
    };

    // Table B: 2 rows: id=2, 3
    std::vector<Tuple> rows_b = {
        {{makeInt(2), makeString("b2")}},
        {{makeInt(3), makeString("b3")}}
    };

    // Swapped: probe is B, build is A, but output is [A][B]
    {
        std::vector<std::unique_ptr<BoundExpr>> probe_keys;
        auto pk = std::make_unique<BoundExpr>();
        pk->kind = BoundExprKind::COLUMN_REF;
        pk->col_idx = 0; // B.id
        pk->affinity = Affinity::NUMERIC;
        probe_keys.push_back(std::move(pk));

        std::vector<std::unique_ptr<BoundExpr>> build_keys;
        auto bk = std::make_unique<BoundExpr>();
        bk->kind = BoundExprKind::COLUMN_REF;
        bk->col_idx = 0; // A.id
        bk->affinity = Affinity::NUMERIC;
        build_keys.push_back(std::move(bk));

        std::vector<size_t> kept = {0, 1};

        HashJoinOperator swapped_join(
            std::make_unique<MockOperator>(rows_b), // probe is B
            std::make_unique<MockOperator>(rows_a), // build is A
            JoinType::INNER,
            /*swapped=*/true,
            std::move(probe_keys),
            std::move(build_keys),
            {Affinity::NUMERIC},
            {Affinity::NUMERIC},
            nullptr,
            /*probe_w=*/2,
            /*build_w=*/2,
            kept,
            /*written_left_w=*/2, // A width
            /*written_right_w=*/2, // B width
            /*is_nested_loop=*/false,
            /*join_stats=*/false
        );

        assert(swapped_join.open());
        Tuple out;
        std::vector<std::string> results;
        while (swapped_join.next(out)) {
            results.push_back(tupleToString(out));
        }
        swapped_join.close();

        assert(results.size() == 2);
        // Tuple layout must be [A][B]
        assert(results[0] == "I:2|S:a2|I:2|S:b2" || results[0] == "I:3|S:a3|I:3|S:b3");
        assert(results[1] == "I:2|S:a2|I:2|S:b2" || results[1] == "I:3|S:a3|I:3|S:b3");
    }

    // Nested-loop fallback: ON a.id < b.id (no equi-keys)
    {
        auto residual = std::make_unique<BoundExpr>();
        residual->kind = BoundExprKind::BINARY_OP;
        residual->op = "<";
        auto l = std::make_unique<BoundExpr>();
        l->kind = BoundExprKind::COLUMN_REF;
        l->col_idx = 0; // a.id
        auto r = std::make_unique<BoundExpr>();
        r->kind = BoundExprKind::COLUMN_REF;
        r->col_idx = 2; // b.id (probe_w = 2, so offset 2 + 0 = 2)
        residual->left = std::move(l);
        residual->right = std::move(r);

        std::vector<size_t> kept = {0, 1};

        HashJoinOperator nl_join(
            std::make_unique<MockOperator>(rows_a), // probe is A
            std::make_unique<MockOperator>(rows_b), // build is B
            JoinType::INNER,
            /*swapped=*/false,
            {},
            {},
            {},
            {},
            std::move(residual),
            2, 2, kept, 2, 2,
            /*is_nested_loop=*/true,
            /*join_stats=*/false
        );

        assert(nl_join.open());
        Tuple out;
        size_t count = 0;
        while (nl_join.next(out)) {
            count++;
            assert(out.values[0].i < out.values[2].i);
        }
        nl_join.close();
        // A.id < B.id: (1, 2), (1, 3), (2, 3) -> 3 rows
        assert(count == 3);
    }

    std::cout << "  -> Swapped join and nested-loop fallback passed!\n";
}

int main() {
    std::cout << "=== Running test_join ===\n";
    testAffinityNormalization();
    testZeroKeptBuildColumns();
    testDuplicateKeys();
    testPushdownLegality();
    testResidualRejectionNullExtended();
    testSwappedRightProbeNullKey();
    testNullKeyStorageAndEmission();
    testEmptyProbeAndBuild();
    testSwappedAndNestedLoop();
    test1000RandomizedPropertyJoins();
    std::cout << "All join unit & property tests PASSED!\n";
    return 0;
}
