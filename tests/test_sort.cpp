#include "../src/ops_sort.h"
#include "../src/ops_distinct.h"
#include "../src/ops_basic.h"
#include "../src/value.h"
#include "../src/group_key.h"

#include <iostream>
#include <cassert>
#include <vector>
#include <random>
#include <algorithm>
#include <string>

// A mock operator that yields a predefined list of tuples
class MockOperator : public Operator {
public:
    MockOperator(std::vector<Tuple> rows, std::vector<std::string> col_names = {"c0", "c1", "c2"})
        : rows_(std::move(rows)), col_names_(std::move(col_names)) {}

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

void testComparator() {
    std::cout << "Testing Value and Sort Comparators...\n";

    // 1. NULL vs Number vs String
    Value v_null = makeNull();
    Value v_int = makeInt(42);
    Value v_str = makeString("hello");

    assert(compareValuesWithNull(v_null, v_null) == 0 && "NULL == NULL");
    assert(compareValuesWithNull(v_null, v_int) < 0 && "NULL < INT");
    assert(compareValuesWithNull(v_int, v_null) > 0 && "INT > NULL");
    assert(compareValuesWithNull(v_null, v_str) < 0 && "NULL < STRING");
    assert(compareValuesWithNull(v_str, v_null) > 0 && "STRING > NULL");
    assert(compareValuesWithNull(v_int, v_str) < 0 && "NUMERIC < STRING");
    assert(compareValuesWithNull(v_str, v_int) > 0 && "STRING > NUMERIC");

    // 2. Numeric 9 before 10
    assert(compareValuesWithNull(makeInt(9), makeInt(10)) < 0 && "9 < 10");
    assert(compareValuesWithNull(makeFloat(9.0), makeInt(10)) < 0 && "9.0 < 10");
    assert(compareValuesWithNull(makeInt(9), makeFloat(10.0)) < 0 && "9 < 10.0");

    // 3. Exact INT vs FLOAT beyond 2^53 (9007199254740992)
    // 2^53 + 1 = 9007199254740993
    long long huge_int = 9007199254740993LL;
    double huge_flt = 9007199254740992.0; // 2^53 can be represented, +1 cannot
    assert(compareValuesWithNull(makeInt(huge_int), makeFloat(huge_flt)) > 0 && "2^53 + 1 > 2^53 double");
    assert(compareValuesWithNull(makeFloat(huge_flt), makeInt(huge_int)) < 0 && "2^53 double < 2^53 + 1 int");

    // 4. Strings by bytes and shorter prefix first
    assert(compareValuesWithNull(makeString("abc"), makeString("abcd")) < 0 && "'abc' < 'abcd'");
    assert(compareValuesWithNull(makeString("abcd"), makeString("abc")) > 0 && "'abcd' > 'abc'");
    assert(compareValuesWithNull(makeString("a"), makeString("b")) < 0 && "'a' < 'b'");
    assert(compareValuesWithNull(makeString("B"), makeString("a")) < 0 && "ASCII 'B' < 'a'");

    // 5. Multi-term mixed ASC/DESC via SortOperator
    std::vector<Tuple> data = {
        {{makeInt(1), makeString("z")}},
        {{makeInt(1), makeString("a")}},
        {{makeInt(2), makeString("m")}},
        {{makeNull(), makeString("b")}}
    };
    // ORDER BY col0 ASC, col1 DESC
    std::vector<SortTerm> terms = {{0, false}, {1, true}};
    auto mock = std::make_unique<MockOperator>(data, std::vector<std::string>{"c0", "c1"});
    SortOperator sorter(std::move(mock), terms, -1);
    assert(sorter.open());

    Tuple out;
    std::vector<Tuple> results;
    while (sorter.next(out)) {
        results.push_back(out);
    }
    sorter.close();

    assert(results.size() == 4);
    // In ASC NULLs first:
    assert(results[0].values[0].type == V_NULL && results[0].values[1].s == "b");
    // col0 == 1: "z" before "a" (col1 DESC)
    assert(results[1].values[0].i == 1 && results[1].values[1].s == "z");
    assert(results[2].values[0].i == 1 && results[2].values[1].s == "a");
    // col0 == 2
    assert(results[3].values[0].i == 2 && results[3].values[1].s == "m");

    std::cout << "  -> Comparator tests OK.\n";
}

void testTopNProperty() {
    std::cout << "Testing Top-N Heap Property (1000 randomized cases)...\n";
    std::mt19937 rng(1337);

    for (int trial = 0; trial < 1000; ++trial) {
        int num_rows = rng() % 201; // 0 to 200 rows
        long long limit = rng() % 31; // 0 to 30
        long long offset = rng() % 31; // 0 to 30
        bool desc = (rng() % 2 == 1);

        std::vector<Tuple> rows;
        for (int r = 0; r < num_rows; ++r) {
            Tuple t;
            // Generate duplicate keys and NULLs
            int key_choice = rng() % 10;
            if (key_choice == 0) {
                t.values.push_back(makeNull());
            } else if (key_choice <= 4) {
                t.values.push_back(makeInt(key_choice)); // duplicates 1..4
            } else {
                t.values.push_back(makeInt(rng() % 50));
            }
            // Second column is unique arrival sequence to break ties
            t.values.push_back(makeInt(r));
            rows.push_back(t);
        }

        std::vector<SortTerm> terms = {{0, desc}};
        std::vector<std::string> cols = {"key", "id"};

        // 1. Run Top-N heap followed by LimitOperator
        auto mock_topn = std::make_unique<MockOperator>(rows, cols);
        long long top_n = limit + offset;
        auto sort_topn = std::make_unique<SortOperator>(std::move(mock_topn), terms, top_n);
        auto limit_topn = std::make_unique<LimitOperator>(std::move(sort_topn), limit, offset);

        assert(limit_topn->open());
        Tuple out;
        std::vector<Tuple> res_topn;
        while (limit_topn->next(out)) {
            res_topn.push_back(out);
        }
        limit_topn->close();

        // 2. Run Full Sort followed by LimitOperator
        auto mock_full = std::make_unique<MockOperator>(rows, cols);
        auto sort_full = std::make_unique<SortOperator>(std::move(mock_full), terms, -1);
        auto limit_full = std::make_unique<LimitOperator>(std::move(sort_full), limit, offset);

        assert(limit_full->open());
        std::vector<Tuple> res_full;
        while (limit_full->next(out)) {
            res_full.push_back(out);
        }
        limit_full->close();

        // Assert Top-N output matches Full Sort output
        assert(res_topn.size() == res_full.size() && "Top-N and Full Sort row counts must match");
        for (size_t i = 0; i < res_topn.size(); ++i) {
            assert(res_topn[i].values[0].type == res_full[i].values[0].type);
            if (res_topn[i].values[0].type == V_INT) {
                assert(res_topn[i].values[0].i == res_full[i].values[0].i);
            }
            assert(res_topn[i].values[1].i == res_full[i].values[1].i && "Tie-break arrival order must match");
        }
    }

    std::cout << "  -> Top-N Heap property tests OK.\n";
}

void testEdgeCases() {
    std::cout << "Testing Edge Cases (LIMIT 0, empty, arrival tie-break, large limit/offset)...\n";

    // 1. LIMIT 0: must not pull any rows but open/close child cleanly
    {
        std::vector<Tuple> rows = {{{makeInt(1)}}, {{makeInt(2)}}};
        auto mock = std::make_unique<MockOperator>(rows, std::vector<std::string>{"c0"});
        MockOperator* mock_ptr = mock.get();
        SortOperator sorter(std::move(mock), {{0, false}}, 0);
        assert(sorter.open());
        assert(mock_ptr->open_called);
        assert(mock_ptr->pull_count == 0 && "LIMIT 0 must not pull any rows from child");
        Tuple t;
        assert(!sorter.next(t) && "LIMIT 0 must produce 0 rows");
        sorter.close();
        assert(mock_ptr->close_called);
    }

    // 2. Empty input
    {
        std::vector<Tuple> empty_rows;
        auto mock = std::make_unique<MockOperator>(empty_rows, std::vector<std::string>{"c0"});
        SortOperator sorter(std::move(mock), {{0, false}}, 10);
        assert(sorter.open());
        Tuple t;
        assert(!sorter.next(t));
        assert(sorter.maxEntriesEverHeld() == 0);
        sorter.close();
    }

    // 3. One row
    {
        std::vector<Tuple> one = {{{makeInt(42)}}};
        auto mock = std::make_unique<MockOperator>(one, std::vector<std::string>{"c0"});
        SortOperator sorter(std::move(mock), {{0, false}}, 5);
        assert(sorter.open());
        Tuple t;
        assert(sorter.next(t));
        assert(t.values[0].i == 42);
        assert(!sorter.next(t));
        sorter.close();
    }

    // 4. All rows equal on key: strict arrival order preservation
    {
        std::vector<Tuple> dupes;
        for (int i = 0; i < 50; ++i) {
            dupes.push_back({{makeInt(7), makeInt(i)}});
        }
        auto mock = std::make_unique<MockOperator>(dupes, std::vector<std::string>{"k", "seq"});
        SortOperator sorter(std::move(mock), {{0, false}}, 20);
        assert(sorter.open());
        for (int i = 0; i < 20; ++i) {
            Tuple t;
            assert(sorter.next(t));
            assert(t.values[0].i == 7);
            assert(t.values[1].i == i && "Tied rows must preserve exact arrival order");
        }
        Tuple dummy;
        assert(!sorter.next(dummy));
        assert(sorter.maxEntriesEverHeld() <= 20 && "Max entries held must not exceed topN");
        sorter.close();
    }

    // 5. Offset larger than row count
    {
        std::vector<Tuple> rows = {{{makeInt(1)}}, {{makeInt(2)}}};
        auto mock = std::make_unique<MockOperator>(rows, std::vector<std::string>{"c0"});
        auto sort = std::make_unique<SortOperator>(std::move(mock), std::vector<SortTerm>{{0, false}}, 10);
        LimitOperator lim(std::move(sort), 5, 10); // offset 10 > 2 rows
        assert(lim.open());
        Tuple t;
        assert(!lim.next(t));
        lim.close();
    }

    // 6. Limit larger than row count
    {
        std::vector<Tuple> rows = {{{makeInt(2)}}, {{makeInt(1)}}};
        auto mock = std::make_unique<MockOperator>(rows, std::vector<std::string>{"c0"});
        auto sort = std::make_unique<SortOperator>(std::move(mock), std::vector<SortTerm>{{0, false}}, 100);
        LimitOperator lim(std::move(sort), 100, 0);
        assert(lim.open());
        Tuple t;
        assert(lim.next(t) && t.values[0].i == 1);
        assert(lim.next(t) && t.values[0].i == 2);
        assert(!lim.next(t));
        lim.close();
    }

    std::cout << "  -> Edge cases OK.\n";
}

void testDistinct() {
    std::cout << "Testing DistinctOperator...\n";

    // 1. ("a", "bc") != ("ab", "c")
    {
        std::vector<Tuple> rows = {
            {{makeString("a"), makeString("bc")}},
            {{makeString("ab"), makeString("c")}}
        };
        auto mock = std::make_unique<MockOperator>(rows, std::vector<std::string>{"c0", "c1"});
        DistinctOperator distinct(std::move(mock), 2);
        assert(distinct.open());
        Tuple t;
        assert(distinct.next(t) && t.values[0].s == "a" && t.values[1].s == "bc");
        assert(distinct.next(t) && t.values[0].s == "ab" && t.values[1].s == "c");
        assert(!distinct.next(t));
        distinct.close();
    }

    // 2. Multiple NULLs collapse into 1 row
    {
        std::vector<Tuple> rows = {
            {{makeNull()}},
            {{makeNull()}},
            {{makeInt(1)}}
        };
        auto mock = std::make_unique<MockOperator>(rows, std::vector<std::string>{"c0"});
        DistinctOperator distinct(std::move(mock), 1);
        assert(distinct.open());
        Tuple t;
        assert(distinct.next(t) && t.values[0].type == V_NULL);
        assert(distinct.next(t) && t.values[0].i == 1);
        assert(!distinct.next(t));
        distinct.close();
    }

    // 3. INT 1 and FLOAT 1.0 are the same row
    {
        std::vector<Tuple> rows = {
            {{makeInt(1)}},
            {{makeFloat(1.0)}},
            {{makeInt(2)}}
        };
        auto mock = std::make_unique<MockOperator>(rows, std::vector<std::string>{"c0"});
        DistinctOperator distinct(std::move(mock), 1);
        assert(distinct.open());
        Tuple t;
        assert(distinct.next(t) && t.values[0].type == V_INT && t.values[0].i == 1);
        assert(distinct.next(t) && t.values[0].type == V_INT && t.values[0].i == 2);
        assert(!distinct.next(t));
        distinct.close();
    }

    // 4. 0.0 and -0.0 same
    {
        std::vector<Tuple> rows = {
            {{makeFloat(0.0)}},
            {{makeFloat(-0.0)}}
        };
        auto mock = std::make_unique<MockOperator>(rows, std::vector<std::string>{"c0"});
        DistinctOperator distinct(std::move(mock), 1);
        assert(distinct.open());
        Tuple t;
        assert(distinct.next(t) && t.values[0].d == 0.0);
        assert(!distinct.next(t));
        distinct.close();
    }

    // 5. First occurrence wins
    {
        std::vector<Tuple> rows = {
            {{makeString("first"), makeInt(1)}},
            {{makeString("first"), makeInt(2)}}
        };
        // visible_width = 1 (distinct on first column only)
        auto mock = std::make_unique<MockOperator>(rows, std::vector<std::string>{"k", "v"});
        DistinctOperator distinct(std::move(mock), 1);
        assert(distinct.open());
        Tuple t;
        assert(distinct.next(t) && t.values[0].s == "first" && t.values[1].i == 1);
        assert(!distinct.next(t));
        distinct.close();
    }

    std::cout << "  -> DistinctOperator tests OK.\n";
}

int main() {
    std::cout << "=== Running test_sort Unit Tests ===\n";
    testComparator();
    testTopNProperty();
    testEdgeCases();
    testDistinct();
    std::cout << "All test_sort tests PASSED!\n";
    return 0;
}

