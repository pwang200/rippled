#ifndef FUZZER_SUITE_H_INCLUDED
#define FUZZER_SUITE_H_INCLUDED

#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/beast/unit_test/runner.h>

namespace fuzzer {

// Minimal runner that discards all output (for performance)
class NullRunner : public beast::unit_test::runner
{
protected:
    void on_suite_begin(beast::unit_test::suite_info const&) override {}
    void on_suite_end() override {}
    void on_case_begin(std::string const&) override {}
    void on_case_end() override {}
    void on_pass() override {}
    void on_fail(std::string const&) override {}
    void on_log(std::string const&) override {}
};

// Minimal suite for fuzzing
class FuzzerSuite : public beast::unit_test::suite
{
private:
    NullRunner runner_;
    void run() override {}

public:
    FuzzerSuite()
    {
        (*this)(runner_);  // Initialize suite
    }
};

} // namespace fuzzer

#endif
