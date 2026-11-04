#pragma once

#include "fault_engine_api.h"
#include "faulttypes.h"

#include <QLibrary>
#include <QString>
#include <QVector>

/* 运行包加载封装：校验 model.json 身份与资产，再按 ABI 1 动态加载
 * libmlk_fault.so。只在工作线程创建、调用和销毁，UI 线程不直接调用引擎。 */
namespace mlk {

struct FaultRuntimeConfig
{
    QString root;                       /* /opt/mlk_fault 或选定的故障包目录 */
    int backend = kFaultBackendCpu;     /* kFaultBackendCpu / kFaultBackendCpuCnnNpu */
};

struct FaultEngineOutcome
{
    bool callOk = false;
    QString error;
    int classIndex = kFaultUnavailable;
    int grade = -1;
    bool hasConfidence = false;
    double confidence = 0.0;
    int backend = kFaultBackendUnknown;
    quint32 inferUs = 0;
};

class FaultRuntime
{
public:
    FaultRuntime() = default;
    ~FaultRuntime();
    FaultRuntime(const FaultRuntime &) = delete;
    FaultRuntime &operator=(const FaultRuntime &) = delete;

    bool open(const FaultRuntimeConfig &config, QString *error);
    void close();
    bool isOpen() const { return m_context != nullptr; }

    bool predict(const QVector<float> &input, FaultEngineOutcome &outcome, QString *error,
                 quint32 excludedClasses = 0);
    bool supportsClassMask() const { return m_predictMasked != nullptr; }

    QString root() const { return m_config.root; }
    QString modelId() const { return m_modelId; }
    QString preprocessId() const { return m_preprocessId; }
    int backend() const { return m_config.backend; }
    /* 运行包内资产不全时就地给出可读原因，调用方转成 error 状态而不是崩溃。 */
    static bool verifyPackage(const QString &root, int backend, QString *modelId,
                              QString *preprocessId, QString *error);

private:
    FaultRuntimeConfig m_config;
    QLibrary m_library;
    QString m_modelId;
    QString m_preprocessId;
    void *m_context = nullptr;
    mlk_fault_predict_fn m_predict = nullptr;
    mlk_fault_predict_masked_fn m_predictMasked = nullptr;
    mlk_fault_destroy_fn m_destroy = nullptr;
    char m_error[512] = {};
};

} // namespace mlk
