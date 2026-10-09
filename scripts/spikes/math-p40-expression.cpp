// P4-0 の製品外試作。式の解釈と点列の所有者を C++ に限定する。
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

struct Node {
    std::string op;
    double value = 0;
    std::unique_ptr<Node> a, b;
};

struct Parser {
    std::string source;
    size_t pos = 0;
    int depth = 0;

    void space() {
        while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' ||
                                       source[pos] == '\r' || source[pos] == '\n'))
            ++pos;
    }

    bool take(char c) {
        space();
        if (pos < source.size() && source[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }

    std::unique_ptr<Node> node(std::string op, std::unique_ptr<Node> a = {},
                               std::unique_ptr<Node> b = {}) {
        return std::make_unique<Node>(Node{std::move(op), 0, std::move(a), std::move(b)});
    }

    std::unique_ptr<Node> sum() {
        auto a = product();
        for (;;) {
            if (take('+'))
                a = node("+", std::move(a), product());
            else if (take('-'))
                a = node("-", std::move(a), product());
            else
                return a;
        }
    }

    std::unique_ptr<Node> product() {
        auto a = unary();
        for (;;) {
            if (take('*'))
                a = node("*", std::move(a), unary());
            else if (take('/'))
                a = node("/", std::move(a), unary());
            else
                return a;
        }
    }

    std::unique_ptr<Node> unary() {
        if (++depth > 64)
            throw std::runtime_error("InvalidExpression");
        std::unique_ptr<Node> a;
        if (take('+'))
            a = node("positive", unary());
        else if (take('-'))
            a = node("negative", unary());
        else {
            a = atom();
            if (take('^'))
                a = node("^", std::move(a), unary());
        }
        --depth;
        return a;
    }

    std::unique_ptr<Node> atom() {
        if (take('(')) {
            auto a = sum();
            if (!take(')'))
                throw std::runtime_error("InvalidExpression");
            return a;
        }
        space();
        const size_t begin = pos;
        if (pos < source.size() &&
            ((source[pos] >= '0' && source[pos] <= '9') || source[pos] == '.')) {
            bool digits = false;
            while (pos < source.size() && source[pos] >= '0' && source[pos] <= '9') {
                ++pos;
                digits = true;
            }
            if (pos < source.size() && source[pos] == '.') {
                ++pos;
                while (pos < source.size() && source[pos] >= '0' && source[pos] <= '9') {
                    ++pos;
                    digits = true;
                }
            }
            if (!digits)
                throw std::runtime_error("InvalidExpression");
            if (pos < source.size() && (source[pos] == 'E' || source[pos] == 'e')) {
                ++pos;
                if (pos < source.size() && (source[pos] == '+' || source[pos] == '-'))
                    ++pos;
                const auto exponent = pos;
                while (pos < source.size() && source[pos] >= '0' && source[pos] <= '9')
                    ++pos;
                if (exponent == pos)
                    throw std::runtime_error("InvalidExpression");
            }
            auto a = node("literal");
            const auto result =
                std::from_chars(source.data() + begin, source.data() + pos, a->value);
            if (result.ec != std::errc{} || result.ptr != source.data() + pos)
                throw std::runtime_error("InvalidExpression");
            if (!std::isfinite(a->value))
                throw std::runtime_error("InvalidExpression");
            return a;
        }
        while (pos < source.size() && source[pos] >= 'a' && source[pos] <= 'z')
            ++pos;
        const auto name = source.substr(begin, pos - begin);
        if (name == "x")
            return node("x");
        if (name == "pi" || name == "e") {
            auto a = node("literal");
            a->value = name == "pi" ? std::numbers::pi : std::numbers::e;
            return a;
        }
        const std::vector<std::string> functions{"sin", "cos", "tan", "exp", "log", "sqrt", "abs"};
        if (name.empty())
            throw std::runtime_error("InvalidExpression");
        if (std::find(functions.begin(), functions.end(), name) == functions.end())
            throw std::runtime_error("UnsupportedExpression");
        if (!take('('))
            throw std::runtime_error("InvalidExpression");
        auto a = sum();
        if (!take(')'))
            throw std::runtime_error("InvalidExpression");
        return node(name, std::move(a));
    }

    std::unique_ptr<Node> parse() {
        if (source.size() > 4096)
            throw std::runtime_error("InvalidExpression");
        auto a = sum();
        space();
        if (pos != source.size())
            throw std::runtime_error("InvalidExpression");
        validateDepth(*a, 1);
        return a;
    }

    void validateDepth(const Node& n, int level) {
        if (level > 64)
            throw std::runtime_error("InvalidExpression");
        if (n.a)
            validateDepth(*n.a, level + 1);
        if (n.b)
            validateDepth(*n.b, level + 1);
    }
};

double evaluate(const Node& n, double x) {
    if (n.op == "literal")
        return n.value;
    if (n.op == "x")
        return x;
    const double a = evaluate(*n.a, x);
    const double b = n.b ? evaluate(*n.b, x) : 0;
    double y = 0;
    if (n.op == "+")
        y = a + b;
    else if (n.op == "-")
        y = a - b;
    else if (n.op == "*")
        y = a * b;
    else if (n.op == "/") {
        if (b == 0)
            throw std::runtime_error("UndefinedDivision");
        y = a / b;
    } else if (n.op == "^")
        y = std::pow(a, b);
    else if (n.op == "positive")
        y = a;
    else if (n.op == "negative")
        y = -a;
    else if (n.op == "sin")
        y = std::sin(a);
    else if (n.op == "cos")
        y = std::cos(a);
    else if (n.op == "tan") {
        if (std::abs(std::cos(a)) < 1e-12)
            throw std::runtime_error("UndefinedDomain");
        y = std::tan(a);
    } else if (n.op == "exp")
        y = std::exp(a);
    else if (n.op == "log") {
        if (a <= 0)
            throw std::runtime_error("UndefinedDomain");
        y = std::log(a);
    } else if (n.op == "sqrt") {
        if (a < 0)
            throw std::runtime_error("UndefinedDomain");
        y = std::sqrt(a);
    } else if (n.op == "abs")
        y = std::abs(a);
    else
        throw std::runtime_error("EvaluationFailure");
    if (!std::isfinite(y))
        throw std::runtime_error(std::isnan(y) ? "UndefinedDomain" : "UndefinedOverflow");
    return y;
}

void require(bool value) {
    if (!value)
        throw std::runtime_error("試作の期待値が一致しません");
}

void tests() {
    int count = 0;
    std::string chain = "x";
    for (int i = 0; i < 63; ++i)
        chain += "+1";
    require(evaluate(*Parser{chain}.parse(), 3) == 66);
    ++count;
    bool deepRejected = false;
    try {
        Parser{chain + "+1"}.parse();
    } catch (const std::runtime_error& e) {
        deepRejected = std::string(e.what()) == "InvalidExpression";
    }
    require(deepRejected);
    ++count;
    for (const auto& [source, expected] :
         std::vector<std::pair<std::string, double>>{{"-x^2", -9},
                                                     {"(-x)^2", 9},
                                                     {"2^3^2", 512},
                                                     {"2^-2", .25},
                                                     {"1+2*3", 7},
                                                     {" sin(pi/2) ", 1},
                                                     {"cos(0)", 1},
                                                     {"exp(0)", 1},
                                                     {"log(e)", 1},
                                                     {"sqrt(9)", 3},
                                                     {"abs(-3)", 3},
                                                     {"+.5e1", 5},
                                                     {"tan(0)", 0}}) {
        auto n = Parser{source}.parse();
        require(std::abs(evaluate(*n, 3) - expected) < 1e-12);
        ++count;
    }
    for (const auto& source : std::vector<std::string>{
             "x+", "foo(x)", "__import__('os')", "x.real", "x[0]", "lambda x:x", "[x for x in x]",
             "open(1)", "sin(x,1)", "2x", "1,5", "1e999", std::string(4097, 'x'),
             std::string(65, '-') + "x"}) {
        bool rejected = false;
        try {
            Parser{source}.parse();
        } catch (...) {
            rejected = true;
        }
        require(rejected);
        ++count;
    }
    for (const auto& source : {"1/x", "sqrt(-1)", "log(0)", "exp(1000)", "(-1)^.5", "tan(pi/2)"}) {
        bool rejected = false;
        try {
            evaluate(*Parser{source}.parse(), 0);
        } catch (...) {
            rejected = true;
        }
        require(rejected);
        ++count;
    }
    std::cout << "{\"passed\":" << count << "}\n";
}

int main(int argc, char** argv) {
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(17);
    try {
        if (argc == 2 && std::string(argv[1]) == "--test") {
            tests();
            return 0;
        }
        if (argc != 6)
            throw std::runtime_error("InvalidGraph");
        const double xmin = std::stod(argv[2]), xmax = std::stod(argv[3]);
        const double ymin = std::stod(argv[4]), ymax = std::stod(argv[5]);
        if (!std::isfinite(xmin) || !std::isfinite(xmax) || !std::isfinite(ymin) ||
            !std::isfinite(ymax) || xmin >= xmax || ymin >= ymax || !std::isfinite(xmax - xmin) ||
            !std::isfinite(ymax - ymin))
            throw std::runtime_error("InvalidViewport");
        auto n = Parser{argv[1]}.parse();
        // 640px の描画領域を 1/4px ごとに検査。区間中央も検査し危険な辺を切る。
        using Point = std::pair<double, double>;
        std::vector<std::vector<Point>> segments;
        std::vector<Point> current;
        int undefined = 0, jumps = 0, finite = 0;
        auto finish = [&] {
            if (current.size() >= 2)
                segments.push_back(current);
            current.clear();
        };
        for (int i = 0; i <= 2560; ++i) {
            const double x = xmin + (xmax - xmin) * i / 2560;
            try {
                const double y = evaluate(*n, x);
                ++finite;
                if (!current.empty()) {
                    const auto [px, py] = current.back();
                    const double mid = evaluate(*n, (px + x) / 2);
                    if (std::abs(y - py) / (ymax - ymin) * 360 > 45 ||
                        std::abs(mid - (py + y) / 2) / (ymax - ymin) * 360 > .5) {
                        finish();
                        ++jumps;
                    }
                }
                current.emplace_back(x, y);
            } catch (...) {
                ++undefined;
                finish();
            }
        }
        finish();
        if (!finite || segments.empty())
            throw std::runtime_error("NoFiniteSamples");
        std::cout << "{\"status\":\"Finite\",\"undefined\":" << undefined << ",\"jumps\":" << jumps
                  << ",\"segments\":[";
        for (size_t i = 0; i < segments.size(); ++i) {
            if (i)
                std::cout << ',';
            std::cout << '[';
            for (size_t j = 0; j < segments[i].size(); ++j) {
                if (j)
                    std::cout << ',';
                std::cout << '[' << segments[i][j].first << ',' << segments[i][j].second << ']';
            }
            std::cout << ']';
        }
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cout << "{\"status\":\"" << e.what() << "\"}\n";
        return 2;
    }
}
