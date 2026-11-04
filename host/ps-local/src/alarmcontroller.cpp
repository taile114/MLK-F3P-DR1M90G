#include "alarmcontroller.h"
#include "pl_host.h"
#include "../driver/pl_ctrl_uapi.h"
#include <cerrno>
#include <cstring>

AlarmController::AlarmController(QObject *parent) : QObject(parent) {
    m_clock.start();
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &AlarmController::update);
    m_timer.start();
}
AlarmController::~AlarmController() {
    if (m_fd >= 0) { pl_set_alarm(m_fd, 0); pl_close(m_fd); }
}
void AlarmController::configure(const mlk::AppConfig &config) { m_config=config; update(); }
void AlarmController::observe(const mlk::Frame &f) {
    if (f.type != mlk::kTypeWaveform || f.ch >= 4 || f.samples.isEmpty()) return;
    auto &last=m_last[f.ch]; last={f.samples.back()};
    if (f.ch==mlk::kVibration && f.samplesY.size()==f.samples.size()
        && f.samplesZ.size()==f.samples.size()) last << f.samplesY.back() << f.samplesZ.back();
    m_seen[f.ch]=m_clock.elapsed();
    // Writes are bounded to 4 Hz; sample ingestion does not generate AXI traffic.
}
void AlarmController::invalidate(int channel) {
    if (channel>=0 && channel<4) { m_seen[channel]=-1; update(); }
}
void AlarmController::modelGrade(int grade) { m_grade=qBound(0,grade,3); update(); }
unsigned AlarmController::desiredState() const {
    unsigned mask=0;
    for (int ch=0;ch<4;++ch) {
        const auto &c=m_config.ch[ch];
        if (!c.alarmEnabled || m_seen[ch]<0 || m_clock.elapsed()-m_seen[ch]>2000) continue;
        const double k=ch==mlk::kCurrent ? 1.0 : ch==mlk::kSpeed ? 0.1 : c.k;
        const double b=(ch==mlk::kCurrent || ch==mlk::kSpeed) ? 0.0 : c.b;
        for (auto raw:m_last[ch]) {
            const double value=raw*k+b;
            if (value<c.alarmLo || value>c.alarmHi) { mask|=1u<<ch; break; }
        }
    }
    return mask | (unsigned(m_grade)<<4);
}
void AlarmController::update() {
    if (m_fd<0) {
        m_fd=pl_open(nullptr);
        uint32_t id=0;
        if (m_fd>=0 && (pl_probe(m_fd,&id) || id!=PL_CTRL_ID_VALUE)) {
            pl_close(m_fd); m_fd=-1; errno=ENOTSUP;
        }
    }
    QString status;
    if (m_fd>=0) {
        if (pl_set_alarm(m_fd,desiredState())) {
            status=QStringLiteral("U7 告警不可用：%1").arg(QString::fromLocal8Bit(std::strerror(errno)));
            pl_close(m_fd); m_fd=-1;
        } else status=QStringLiteral("U7 声光告警可用");
    } else status=QStringLiteral("U7 告警不可用（需要 PL 接口 v1.3）");
    if (status!=m_status) { m_status=status; emit hardwareStatus(status); }
}
