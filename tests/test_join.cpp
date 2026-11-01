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

    std::vector<JoinClause> joins;

    // Empty joins: table 0 is legal
    assert(isTablePushdownLegal(0, joins) == true);

    // Join 0 is INNER
    JoinClause j0;
    j0.type = JoinType::INNER;
    joins.push_back(std::move(j0));

    assert(isTablePushdownLegal(0, joins) == true);
    assert(isTablePushdownLegal(1, joins) == true);

    // Join 1 is LEFT
    JoinClause j1;
    j1.type = JoinType::LEFT;
    joins.push_back(std::move(j1));

    assert(isTablePushdownLegal(0, joins) == true);
    assert(isTablePushdownLegal(1, joins) == true);
    assert(isTablePushdownLegal(2, joins) == false && "Right side of LEFT join is NOT legal for pushdown");

    // Join 2 is INNER
    JoinClause j2;
    j2.type = JoinType::INNER;
    joins.push_back(std::move(j2));

    assert(isTablePushdownLegal(3, joins) == true && "Table introduced by INNER join is legal");

    std::cout << "  -> Pushdown legality truth table passed!\n";
}

// -----------------------------------------------------------------------------
// 5. 1000 Randomized Property Tests vs Reference In-Test Join
// -----------------------------------------------------------------------------
void test1000RandomizedPropertyJoins() {
    std::cout << "Running 1000 Randomized Property Tests vs Reference Nested-Loop Join...\n";

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> probe_len_dist(0, 40);
    std::uniform_int_distribution<size_t> build_len_dist(0, 40);
    std::uniform_int_distribution<int> key_dist(1, 15);
    std::uniform_int_distribution<int> null_dist(0, 8); // 0 means NULL
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_int_distribution<int> type_dist(0, 2); // 0 = INT, 1 = FLOAT, 2 = STRING

    for (int iter = 0; iter < 1000; ++iter) {
        size_t p_len = probe_len_dist(rng);
        size_t b_len = build_len_dist(rng);
        bool is_left = (bool_dist(rng) == 1);
        bool has_residual = (bool_dist(rng) == 1);
        int key_type = type_dist(rng);

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
        for (size_t p = 0; p < probe_tuples.size(); ++p) {
            const auto& pt = probe_tuples[p];
            bool matched = false;
            if (pt.values[0].type != V_NULL) {
                for (size_t b = 0; b < build_tuples.size(); ++b) {
                    const auto& bt = build_tuples[b];
                    if (bt.values[0].type == V_NULL) continue;

                    bool keys_equal = false;
                    if (key_type == 0) keys_equal = (pt.values[0].i == bt.values[0].i);
                    else if (key_type == 1) keys_equal = (pt.values[0].d == bt.values[0].d);
                    else keys_equal = (pt.values[0].s == bt.values[0].s);

                    if (keys_equal) {
                        bool res_pass = true;
                        if (has_residual) {
                            // Residual condition: probe.payload <= build.payload
                            res_pass = (pt.values[1].i <= bt.values[1].i);
                        }
                        if (res_pass) {
                            matched = true;
                            Tuple combined;
                            combined.values = {pt.values[0], pt.values[1], bt.values[0], bt.values[1]};
                            ref_multiset[tupleToString(combined)]++;
                        }
                    }
                }
            }
            if (!matched && is_left) {
                Tuple combined;
                combined.values = {pt.values[0], pt.values[1], makeNull(), makeNull()};
                ref_multiset[tupleToString(combined)]++;
            }
        }

        // Run HashJoinOperator
        std::vector<std::unique_ptr<BoundExpr>> p_keys;
        auto pk = std::make_unique<BoundExpr>();
        pk->kind = BoundExprKind::COLUMN_REF;
        pk->col_idx = 0;
        pk->affinity = (key_type == 2 ? Affinity::TEXT : Affinity::NUMERIC);
        p_keys.push_back(std::move(pk));

        std::vector<std::unique_ptr<BoundExpr>> b_keys;
        auto bk = std::make_unique<BoundExpr>();
        bk->kind = BoundExprKind::COLUMN_REF;
        bk->col_idx = 0;
        bk->affinity = (key_type == 2 ? Affinity::TEXT : Affinity::NUMERIC);
        b_keys.push_back(std::move(bk));

        std::unique_ptr<BoundExpr> residual = nullptr;
        if (has_residual) {
            residual = std::make_unique<BoundExpr>();
            residual->kind = BoundExprKind::BINARY_OP;
            residual->op = "<=";
            auto l = std::make_unique<BoundExpr>();
            l->kind = BoundExprKind::COLUMN_REF;
            l->col_idx = 1; // probe.payload
            auto r = std::make_unique<BoundExpr>();
            r->kind = BoundExprKind::COLUMN_REF;
            r->col_idx = 3; // build.payload (probe_width = 2, so offset 2 + 1 = 3)
            residual->left = std::move(l);
            residual->right = std::move(r);
        }

        std::vector<size_t> kept = {0, 1};

        HashJoinOperator join_op(
            std::make_unique<MockOperator>(probe_tuples),
            std::make_unique<MockOperator>(build_tuples),
            is_left ? JoinType::LEFT : JoinType::INNER,
            /*swapped=*/false,
            std::move(p_keys),
            std::move(b_keys),
            {key_type == 2 ? Affinity::TEXT : Affinity::NUMERIC},
            {key_type == 2 ? Affinity::TEXT : Affinity::NUMERIC},
            std::move(residual),
            2, 2, kept, 2, 2, false, false
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
                      << ", is_left=" << is_left << ", has_residual=" << has_residual << ", key_type=" << key_type << ")\n";
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
    testSwappedAndNestedLoop();
    test1000RandomizedPropertyJoins();
    std::cout << "All join unit & property tests PASSED!\n";
    return 0;
}
