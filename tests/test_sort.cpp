#include "../src/ops_sort.h"
#include "../src/ops_distinct.h"
#include "../src/ops_basic.h"
#include "../src/value.h"
#include "../src/group_key.h"

#include <iostream>
#include <cassert>
#include <vector>
#include <deque>
#include <random>
#include <algorithm>
#include <string>
#include <fcntl.h>
#include <unistd.h>

// A mock operator that yields a predefined list of tuples
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

void testExternalSort() {
    std::cout << "Testing External Merge Sort...\n";

    // Helper lambda to compare in-memory sort vs external sort output
    auto runAndCompare = [](
        const std::vector<Tuple>& input_data,
        const std::vector<SortTerm>& terms,
        size_t mem_limit,
        size_t fan_in,
        size_t* out_runs = nullptr,
        size_t* out_passes = nullptr
    ) {
        // 1. Run in-memory sort (large limit)
        std::vector<Tuple> in_mem_rows;
        std::deque<std::string> in_mem_arena;
        {
            auto mock = std::make_unique<MockOperator>(input_data);
            SortOperator sorter(std::move(mock), terms, -1, 1024ULL * 1024 * 1024);
            assert(sorter.open());
            Tuple t;
            while (sorter.next(t)) {
                Tuple copy;
                for (const auto& v : t.values) {
                    if (v.type == V_STRING) {
                        in_mem_arena.push_back(std::string(v.s));
                        copy.values.push_back(makeString(in_mem_arena.back()));
                    } else {
                        copy.values.push_back(v);
                    }
                }
                in_mem_rows.push_back(std::move(copy));
            }
            sorter.close();
        }

        // 2. Run external sort (small limit)
        std::vector<Tuple> ext_rows;
        std::deque<std::string> ext_arena;
        {
            auto mock = std::make_unique<MockOperator>(input_data);
            SortOperator sorter(std::move(mock), terms, -1, mem_limit, "/tmp", false, fan_in);
            assert(sorter.open());
            if (out_runs) *out_runs = sorter.initialRunsCount();
            if (out_passes) *out_passes = sorter.mergePassesCount();

            Tuple t;
            while (sorter.next(t)) {
                Tuple copy;
                for (const auto& v : t.values) {
                    if (v.type == V_STRING) {
                        ext_arena.push_back(std::string(v.s));
                        copy.values.push_back(makeString(ext_arena.back()));
                    } else {
                        copy.values.push_back(v);
                    }
                }
                ext_rows.push_back(std::move(copy));
            }
            sorter.close();
        }

        assert(in_mem_rows.size() == ext_rows.size() && "Row count mismatch");
        for (size_t r = 0; r < in_mem_rows.size(); ++r) {
            const auto& row_a = in_mem_rows[r].values;
            const auto& row_b = ext_rows[r].values;
            assert(row_a.size() == row_b.size() && "Column count mismatch");
            for (size_t c = 0; c < row_a.size(); ++c) {
                if (row_a[c].type != row_b[c].type) {
                    std::cerr << "Mismatch at row " << r << " col " << c << " type: in_mem="
                              << (int)row_a[c].type << " ext=" << (int)row_b[c].type << "\n";
                    assert(false);
                }
                switch (row_a[c].type) {
                    case V_NULL:
                        break;
                    case V_INT:
                        if (row_a[c].i != row_b[c].i) {
                            std::cerr << "Mismatch at row " << r << " col " << c << " INT: in_mem="
                                      << row_a[c].i << " ext=" << row_b[c].i << "\n";
                            assert(false);
                        }
                        break;
                    case V_FLOAT:
                        if (row_a[c].d != row_b[c].d) {
                            std::cerr << "Mismatch at row " << r << " col " << c << " FLOAT: in_mem="
                                      << row_a[c].d << " ext=" << row_b[c].d << "\n";
                            assert(false);
                        }
                        break;
                    case V_STRING:
                        if (row_a[c].s != row_b[c].s) {
                            std::cerr << "Mismatch at row " << r << " col " << c << " STRING: in_mem='"
                                      << row_a[c].s << "' ext='" << row_b[c].s << "'\n";
                            assert(false);
                        }
                        break;
                }
            }
        }
    };

    // 1. Empty input
    std::cout << "  Subtest 1: Empty input\n";
    {
        std::vector<Tuple> empty_data;
        std::vector<SortTerm> terms = {{0, false}};
        size_t runs = 0, passes = 0;
        runAndCompare(empty_data, terms, 100, 64, &runs, &passes);
        assert(runs == 0 && passes == 0 && "Empty input should produce 0 runs");
    }

    // 2. Exactly 1 run
    std::cout << "  Subtest 2: Exactly 1 run\n";
    {
        std::vector<Tuple> data = {
            {{makeInt(5), makeString("e")}},
            {{makeInt(2), makeString("b")}},
            {{makeInt(4), makeString("d")}},
            {{makeInt(1), makeString("a")}},
            {{makeInt(3), makeString("c")}}
        };
        std::vector<SortTerm> terms = {{0, false}};
        std::vector<Tuple> two_rows = {
            {{makeInt(2)}}, {{makeInt(1)}}
        };
        size_t runs = 0, passes = 0;
        runAndCompare(two_rows, {{0, false}}, 60, 64, &runs, &passes);
        assert(runs == 2 && "Expected 2 initial runs");
    }

    // Exactly 1 run case: 1 row with limit forcing run creation
    std::cout << "  Subtest 2b: Manual 1 run\n";
    {
        int fd = ::open("/tmp", O_TMPFILE | O_RDWR, 0600);
        if (fd < 0) {
            char tpath[] = "/tmp/test_1run_XXXXXX";
            fd = ::mkstemp(tpath);
            ::unlink(tpath);
        }
        RunWriter writer(fd);
        Value v1 = makeInt(10);
        Value v2 = makeInt(20);
        writer.writeRecord(0, &v1, 1);
        writer.writeRecord(1, &v2, 1);
        writer.flush();
        ::lseek(fd, 0, SEEK_SET);

        RunReader reader(fd, 1);
        assert(reader.nextRecord());
        assert(reader.currentValues()[0].i == 10);
        assert(reader.nextRecord());
        assert(reader.currentValues()[0].i == 20);
        assert(!reader.nextRecord());
        reader.close();
    }

    // 3. 200 KB string (exceeds 64 KB read buffer, tests RunReader resize)
    std::cout << "  Subtest 3: 200 KB string\n";
    {
        std::string big_str1(200000, 'A');
        std::string big_str2(200000, 'B');
        big_str1[1000] = 'X';
        big_str2[1000] = 'Y';

        std::vector<Tuple> data = {
            {{makeInt(2), makeString(big_str2)}},
            {{makeInt(1), makeString(big_str1)}}
        };
        // Small memory limit so each row spills to disk
        runAndCompare(data, {{0, false}}, 1000, 64);
    }

    // 4. Empty strings and strings with commas, quotes, newlines
    std::cout << "  Subtest 4: String edge cases\n";
    {
        std::vector<Tuple> data = {
            {{makeString(""), makeInt(1)}},
            {{makeString("hello,world"), makeInt(2)}},
            {{makeString("line1\nline2\r\nline3"), makeInt(3)}},
            {{makeString("\"quoted\" string with, commas"), makeInt(4)}},
            {{makeString(""), makeInt(5)}},
            {{makeString(",,,,\"\"\"\"\n\n"), makeInt(6)}}
        };
        runAndCompare(data, {{0, false}, {1, true}}, 120, 64);
    }

    // 5. INT 1 vs FLOAT 1.0 (types preserved across runs)
    std::cout << "  Subtest 5: INT 1 vs FLOAT 1.0\n";
    {
        std::vector<Tuple> data = {
            {{makeInt(1), makeString("int_one")}},
            {{makeFloat(1.0), makeString("float_one")}},
            {{makeInt(2), makeString("int_two")}},
            {{makeFloat(2.0), makeString("float_two")}}
        };
        // Sort by col 0 ASC, col 1 ASC with small memory limit (forces runs)
        runAndCompare(data, {{0, false}, {1, false}}, 100, 64);
    }

    // 6. Duplicate keys across many runs (stable arrival order)
    std::cout << "  Subtest 6: Duplicate keys arrival\n";
    {
        std::vector<Tuple> data;
        for (int i = 0; i < 100; ++i) {
            data.push_back({{makeInt(42), makeInt(i)}});
        }
        size_t runs = 0;
        runAndCompare(data, {{0, false}}, 200, 64, &runs);
        assert(runs >= 40 && "Expected many runs for duplicate key arrival test");
    }

    // 7. More than 64 runs with fan-in 3 (exercises multi-pass leveled merges)
    std::cout << "  Subtest 7: >64 runs with fan-in 3\n";
    {
        std::vector<Tuple> data;
        std::mt19937 rng(42);
        for (int i = 0; i < 200; ++i) {
            data.push_back({{makeInt(static_cast<long long>(rng() % 1000)), makeString("val_" + std::to_string(i))}});
        }
        // Memory limit 200 bytes -> batch capacity ~2 rows -> ~100 runs!
        // With fan_in = 3, requires log3(100) ~ 5 levels of merge passes!
        size_t runs = 0, passes = 0;
        runAndCompare(data, {{0, false}, {1, false}}, 200, 3, &runs, &passes);
        assert(runs > 64 && "Expected > 64 runs");
        assert(passes > 1 && "Expected multi-pass merge with fan-in 3");
    }

    std::cout << "  -> External Merge Sort tests OK.\n";
}

int main() {
    std::cout << "=== Running test_sort Unit Tests ===\n";
    testComparator();
    testTopNProperty();
    testEdgeCases();
    testDistinct();
    testExternalSort();
    std::cout << "All test_sort tests PASSED!\n";
    return 0;
}

