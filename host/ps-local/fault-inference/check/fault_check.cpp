/* 运行包检查程序：通过共享库 ABI 回放冻结 test 集，核对分类/分级与参考结果。
 *
 * 用法：fault_check ROOT [--data DIR] [--library FILE] [--limit N] [--report FILE]
 *   ROOT      运行包目录（weights.bin、weights.tsv、thresholds.bin、libmlk_fault.so）
 *   --data    含 test_inputs.bin 与 test_index.csv 的目录，默认 ROOT/data
 *   --library 指定共享库，默认 ROOT/libmlk_fault.so（Windows 为 libmlk_fault.dll）
 *   --limit   只回放前 N 窗，默认全部
 *   --report  写 JSON 报告，默认 DIR/report.json
 *
 * 判定：与 test_index.csv 的 reference_class/reference_grade 不一致即失败；
 * 分类准确率与联合准确率写入报告，供数据集评估对照。
 */
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../../common/fault_engine_api.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr int kClassCount = 5;
constexpr unsigned kInputFloats = MLK_FAULT_INPUT_FLOATS;

struct Library {
#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void *handle = nullptr;
#endif
    mlk_fault_abi_fn abi = nullptr;
    mlk_fault_create_fn create = nullptr;
    mlk_fault_predict_fn predict = nullptr;
    mlk_fault_destroy_fn destroy = nullptr;

    bool open(const std::string &path, std::string &error)
    {
#ifdef _WIN32
        handle = LoadLibraryA(path.c_str());
        if (!handle) { error = "cannot load library: " + path; return false; }
        const auto resolve = [this](const char *name) {
            return reinterpret_cast<void *>(GetProcAddress(handle, name));
        };
        abi = reinterpret_cast<mlk_fault_abi_fn>(resolve("mlk_fault_abi"));
        create = reinterpret_cast<mlk_fault_create_fn>(resolve("mlk_fault_create"));
        predict = reinterpret_cast<mlk_fault_predict_fn>(resolve("mlk_fault_predict"));
        destroy = reinterpret_cast<mlk_fault_destroy_fn>(resolve("mlk_fault_destroy"));
#else
        handle = dlopen(path.c_str(), RTLD_NOW);
        if (!handle) { error = std::string("cannot load library: ") + dlerror(); return false; }
        abi = reinterpret_cast<mlk_fault_abi_fn>(dlsym(handle, "mlk_fault_abi"));
        create = reinterpret_cast<mlk_fault_create_fn>(dlsym(handle, "mlk_fault_create"));
        predict = reinterpret_cast<mlk_fault_predict_fn>(dlsym(handle, "mlk_fault_predict"));
        destroy = reinterpret_cast<mlk_fault_destroy_fn>(dlsym(handle, "mlk_fault_destroy"));
#endif
        if (!abi || !create || !predict || !destroy) { error = "library misses ABI symbols"; return false; }
        return true;
    }
    ~Library()
    {
#ifdef _WIN32
        if (handle) FreeLibrary(handle);
#else
        if (handle) dlclose(handle);
#endif
    }
};

template <class T>
std::vector<T> readBinary(const std::string &path, std::string &error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) { error = "missing file: " + path; return {}; }
    const auto size = file.tellg();
    if (size <= 0 || size % static_cast<std::streamoff>(sizeof(T))) {
        error = "invalid file size: " + path;
        return {};
    }
    std::vector<T> values(static_cast<size_t>(size) / sizeof(T));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
    if (!file) { error = "read failed: " + path; return {}; }
    return values;
}

std::vector<std::string> split(const std::string &line)
{
    std::vector<std::string> fields;
    std::string field;
    for (char c : line) {
        if (c == ',') { fields.push_back(field); field.clear(); }
        else if (c != '\r') field.push_back(c);
    }
    fields.push_back(field);
    return fields;
}

struct Row {
    int target_class = -1;
    int target_grade = -1;
    int reference_class = -1;
    int reference_grade = -1;
};

bool readIndex(const std::string &path, std::vector<Row> &rows, std::string &error)
{
    std::ifstream file(path);
    if (!file) { error = "missing file: " + path; return false; }
    std::string line;
    if (!std::getline(file, line)) { error = "empty index file"; return false; }
    const auto header = split(line);
    std::map<std::string, size_t> column;
    for (size_t i = 0; i < header.size(); ++i) column[header[i]] = i;
    for (const char *name : {"class_target", "grade_target", "reference_class", "reference_grade"}) {
        if (!column.count(name)) { error = std::string("index misses column ") + name; return false; }
    }
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        const auto fields = split(line);
        if (fields.size() != header.size()) { error = "index column count mismatch"; return false; }
        Row row;
        try {
            const auto number = [&fields, &column](const char *name) {
                const auto &text = fields[column.at(name)];
                size_t used = 0;
                const int value = std::stoi(text, &used);
                if (used != text.size()) throw std::runtime_error("invalid index integer");
                return value;
            };
            row.target_class = number("class_target");
            row.target_grade = number("grade_target");
            row.reference_class = number("reference_class");
            row.reference_grade = number("reference_grade");
        } catch (const std::exception &) { error = "invalid index integer"; return false; }
        const auto valid = [](int cls, int grade) {
            return cls >= 0 && cls < kClassCount
                && (cls == 0 ? grade == 0 : grade >= 1 && grade <= 3);
        };
        if (!valid(row.target_class, row.target_grade) || !valid(row.reference_class, row.reference_grade)) {
            error = "index class/grade out of range"; return false;
        }
        rows.push_back(row);
    }
    if (rows.empty()) { error = "index has no samples"; return false; }
    return true;
}

std::string jsonEscape(const std::string &text)
{
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (static_cast<unsigned char>(c) < 0x20) out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    std::string root, data, library, report, predictions;
    int backend = MLK_FAULT_CPU;
    unsigned limit = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const bool hasNext = i + 1 < argc;
        if (argument == "--data" && hasNext) data = argv[++i];
        else if (argument == "--library" && hasNext) library = argv[++i];
        else if (argument == "--limit" && hasNext) {
            const std::string value = argv[++i];
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) return 2;
            try {
                const auto parsed = std::stoull(value);
                if (parsed > 1000000) return 2;
                limit = static_cast<unsigned>(parsed);
            } catch (const std::exception &) { return 2; }
        }
        else if (argument == "--report" && hasNext) report = argv[++i];
        else if (argument == "--predictions" && hasNext) predictions = argv[++i];
        else if (argument == "--backend" && hasNext) {
            const std::string name = argv[++i];
            if (name == "cpu") backend = MLK_FAULT_CPU;
            else if (name == "npu") backend = MLK_FAULT_CPU_CNN_NPU;
            else { std::fprintf(stderr, "unknown backend: %s\n", name.c_str()); return 2; }
        }
        else if (argument.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown option: %s\n", argument.c_str()); return 2; }
        else if (root.empty()) root = argument;
    }
    if (root.empty()) {
        std::fprintf(stderr, "usage: fault_check ROOT [--backend cpu|npu] [--data DIR] [--library FILE] [--limit N] [--report FILE]\n");
        return 2;
    }
    if (data.empty()) data = root + "/data";
    if (report.empty()) report = data + "/fault_check_report.json";
#ifdef _WIN32
    if (library.empty()) library = root + "/libmlk_fault.dll";
#else
    if (library.empty()) library = root + "/libmlk_fault.so";
#endif

    std::string error;
    Library lib;
    if (!lib.open(library, error)) {
        std::printf("{\"result\":\"failed\",\"stage\":\"library\",\"error\":\"%s\"}\n", jsonEscape(error).c_str());
        return 3;
    }
    if (lib.abi() != MLK_FAULT_ENGINE_ABI) {
        std::printf("{\"result\":\"failed\",\"stage\":\"abi\",\"error\":\"unexpected ABI %d\"}\n", lib.abi());
        return 3;
    }

    char message[512] = {0};
    void *context = lib.create(root.c_str(), backend, message, sizeof(message));
    if (!context) {
        std::printf("{\"result\":\"failed\",\"stage\":\"create\",\"error\":\"%s\"}\n", jsonEscape(message).c_str());
        return 3;
    }

    const auto inputs = readBinary<float>(data + "/test_inputs.bin", error);
    std::vector<Row> rows;
    if (inputs.empty() || !readIndex(data + "/test_index.csv", rows, error)) {
        lib.destroy(context);
        std::printf("{\"result\":\"failed\",\"stage\":\"dataset\",\"error\":\"%s\"}\n", jsonEscape(error).c_str());
        return 3;
    }
    if (inputs.size() != rows.size() * kInputFloats) {
        lib.destroy(context);
        std::printf("{\"result\":\"failed\",\"stage\":\"dataset\",\"error\":\"input count mismatch\"}\n");
        return 3;
    }

    const unsigned total = limit ? std::min<unsigned>(limit, static_cast<unsigned>(rows.size()))
                                 : static_cast<unsigned>(rows.size());
    unsigned class_correct = 0, joint_correct = 0, reference_mismatch = 0;
    unsigned reported_backend = 0;
    double total_us = 0.0;
    std::vector<unsigned> latencies;
    std::ofstream predictionFile;
    if (!predictions.empty()) {
        predictionFile.open(predictions);
        if (!predictionFile) { lib.destroy(context); return 3; }
        predictionFile << "sample,class_index,grade,confidence,backend,infer_us\n" << std::setprecision(9);
    }
    std::vector<std::array<int, kClassCount>> confusion(kClassCount);
    for (auto &row : confusion) row.fill(0);

    for (unsigned i = 0; i < total; ++i) {
        mlk_fault_result result{};
        result.struct_size = 32;
        if (lib.predict(context, inputs.data() + static_cast<size_t>(i) * kInputFloats, kInputFloats,
                        &result, message, sizeof(message)) != 0 || result.valid != 1
            || result.struct_size != sizeof(result) || result.reserved != 0
            || result.backend != static_cast<unsigned>(backend)
            || result.class_index < 0 || result.class_index >= kClassCount
            || (result.class_index == 0 ? result.grade != 0 : result.grade < 1 || result.grade > 3)
            || !std::isfinite(result.confidence) || result.confidence < 0.f || result.confidence > 1.f) {
            lib.destroy(context);
            std::printf("{\"result\":\"failed\",\"stage\":\"predict\",\"sample\":%u,\"error\":\"%s\"}\n",
                        i, jsonEscape(message).c_str());
            return 3;
        }
        total_us += result.infer_us;
        latencies.push_back(result.infer_us);
        if (predictionFile.is_open())
            predictionFile << i << ',' << result.class_index << ',' << result.grade << ','
                << result.confidence << ',' << result.backend << ',' << result.infer_us << '\n';
        if (!reported_backend) reported_backend = result.backend;
        const Row &row = rows[i];
        class_correct += result.class_index == row.target_class;
        joint_correct += (result.class_index == row.target_class) && (result.grade == row.target_grade);
        reference_mismatch += (result.class_index != row.reference_class) || (result.grade != row.reference_grade);
        confusion[row.target_class][result.class_index] += 1;
    }
    lib.destroy(context);
    if (predictionFile.is_open()) {
        predictionFile.flush();
        if (!predictionFile) return 3;
    }
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&latencies](double ratio) {
        return latencies[static_cast<size_t>(std::ceil(ratio * latencies.size())) - 1];
    };

    double macro = 0.0;
    int classes = 0;
    std::string matrix;
    for (int truth = 0; truth < kClassCount; ++truth) {
        int row_total = 0, row_correct = 0;
        for (int predicted = 0; predicted < kClassCount; ++predicted) {
            row_total += confusion[truth][predicted];
            if (truth == predicted) row_correct += confusion[truth][predicted];
        }
        if (row_total) { macro += double(row_correct) / row_total; ++classes; }
        if (truth) matrix += ",";
        matrix += "[";
        for (int predicted = 0; predicted < kClassCount; ++predicted) {
            if (predicted) matrix += ",";
            matrix += std::to_string(confusion[truth][predicted]);
        }
        matrix += "]";
    }
    if (classes) macro /= classes;

    std::ofstream out(report);
    if (!out) { std::fprintf(stderr, "cannot write report: %s\n", report.c_str()); return 3; }
    {
        out << "{\n"
            << "  \"schema\": \"mlk.fault.runtime_check.v1\",\n"
            << "  \"root\": \"" << jsonEscape(root) << "\",\n"
            << "  \"samples\": " << total << ",\n"
            << "  \"backend_requested\": " << backend << ",\n"
            << "  \"backend_reported\": " << reported_backend << ",\n"
            << "  \"class_correct\": " << class_correct << ",\n"
            << "  \"joint_correct\": " << joint_correct << ",\n"
            << "  \"class_accuracy\": " << (double(class_correct) / total) << ",\n"
            << "  \"joint_accuracy\": " << (double(joint_correct) / total) << ",\n"
            << "  \"macro_class_accuracy\": " << macro << ",\n"
            << "  \"reference_mismatch\": " << reference_mismatch << ",\n"
            << "  \"mean_infer_us\": " << (total_us / total) << ",\n"
            << "  \"median_infer_us\": " << percentile(0.5) << ",\n"
            << "  \"p95_infer_us\": " << percentile(0.95) << ",\n"
            << "  \"max_infer_us\": " << latencies.back() << ",\n"
            << "  \"confusion_target_by_prediction\": [" << matrix << "]\n"
            << "}\n";
    }
    out.flush();
    if (!out) return 3;

    std::printf("{\"result\":\"%s\",\"samples\":%u,\"class_correct\":%u,\"joint_correct\":%u,"
                "\"reference_mismatch\":%u,\"backend\":%u,\"mean_infer_us\":%.3f,\"report\":\"%s\"}\n",
                reference_mismatch ? "reference_mismatch" : "completed", total, class_correct,
                joint_correct, reference_mismatch, reported_backend, total_us / total,
                jsonEscape(report).c_str());
    return reference_mismatch ? 70 : 0;
}
