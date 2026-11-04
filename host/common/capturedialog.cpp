#include "capturedialog.h"
#include "workorders.h"
#include "faulttypes.h"
#include "../remote-pc/src/faultstore.h"
#include <QtWidgets>
#include <algorithm>
#include <cmath>
#include <limits>

namespace mlk {
namespace {
QString captureLabelText(const QJsonObject &label) {
    const int cls=label.value("classIndex").toInt(kFaultUnavailable);
    if (cls>=kFaultOverload && cls<=kFaultMountTilt)
        return QStringLiteral("%1 · 等级 %2%3").arg(faultDisplayName(cls)).arg(label.value("grade").toInt())
            .arg(label.value("scope").toString()==QLatin1String("model_window")?QStringLiteral("（模型窗口）"):QString());
    return label.value("label").toString();
}
}
class CaptureTable : public QAbstractTableModel {
public:
    explicit CaptureTable(QObject *parent):QAbstractTableModel(parent) {}
    QVector<CaptureRow> values;
    int rowCount(const QModelIndex &p={}) const override { return p.isValid()?0:values.size(); }
    int columnCount(const QModelIndex &p={}) const override { return p.isValid()?0:9; }
    QVariant data(const QModelIndex &index,int role) const override {
        if (!index.isValid() || role!=Qt::DisplayRole) return {};
        const auto &row=values[index.row()];
        if (!index.column()) return QString::number(row.tsUs);
        if (index.column()==8) return row.validMask;
        const double v=row.value[index.column()-1];
        return std::isfinite(v)?QString::number(v,'g',9):QStringLiteral("不可用");
    }
    QVariant headerData(int section,Qt::Orientation orientation,int role) const override {
        if (role!=Qt::DisplayRole) return {};
        if (orientation==Qt::Vertical) return section+1;
        const QStringList names={"UTC / µs",QStringLiteral("温度 / ℃"),QStringLiteral("电流 / mA"),
            QStringLiteral("转速 / rpm"),"X / g","Y / g","Z / g",QStringLiteral("合成 / g"),"valid_mask"};
        return names.value(section);
    }
    void replace(QVector<CaptureRow> rows) { beginResetModel(); values=std::move(rows); endResetModel(); }
};

// Time coordinates are shared by all four plots. Per-pixel envelopes retain
// spikes when a whole segment contains more samples than screen pixels.
class CapturePlot : public QWidget {
public:
    explicit CapturePlot(int channel,QWidget *parent):QWidget(parent),m_channel(channel) {
        setMinimumSize(220,140); setObjectName(QStringLiteral("capturePlot%1").arg(channel));
    }
    const CaptureSeries *values=nullptr;
    QJsonArray labels;
    quint64 start=0,end=1,origin=0;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.fillRect(rect(),QColor("#11161b"));
        const QString names[]={QStringLiteral("温度 / ℃"),QStringLiteral("电流 / mA"),
            QStringLiteral("转速 / rpm"),QStringLiteral("振动 XYZ / g")};
        p.setPen(QColor("#c9d1d9")); p.drawText(10,20,names[m_channel]);
        const QRectF r=QRectF(rect()).adjusted(80,32,-15,-28);
        p.setPen(QColor("#303b45")); p.drawRect(r);
        for (int i=1;i<5;++i) p.drawLine(QPointF(r.left()+r.width()*i/5,r.top()),QPointF(r.left()+r.width()*i/5,r.bottom()));
        p.setPen(QColor("#8b949e"));
        p.drawText(QPointF(r.left(),height()-7),QStringLiteral("%1 s").arg(double(start-origin)/1e6,0,'f',3));
        p.drawText(QRectF(r.right()-110,height()-23,110,20),Qt::AlignRight,
            QStringLiteral("%1 s").arg(double(end-origin)/1e6,0,'f',3));
        if (!values || end<=start) return;
        p.save(); p.setClipRect(r);
        for (const auto &value:labels) {
            const auto label=value.toObject();
            const auto begin=label.value("startUs").toString().toULongLong();
            const auto finish=label.value("endUs").toString().toULongLong();
            if (finish<=start || begin>=end) continue;
            const double x=r.left()+double(qMax(begin,start)-start)*r.width()/double(end-start);
            const double right=r.left()+double(qMin(finish,end)-start)*r.width()/double(end-start);
            p.fillRect(QRectF(x,r.top(),qMax(2.0,right-x),r.height()),
                label.value("abnormal").toBool()?QColor(230,90,90,45):QColor(63,185,80,45));
            p.setPen(QColor("#ff9c9c"));
            p.drawText(QRectF(x+3,r.top()+2,r.width(),18),captureLabelText(label));
        }
        p.restore();
        const int first=m_channel==3?3:m_channel, last=m_channel==3?5:first;
        double lo=std::numeric_limits<double>::infinity(),hi=-lo;
        const auto firstPoint=[&](const QVector<CapturePoint> &points) {
            return std::lower_bound(points.begin(),points.end(),start,
                [](const CapturePoint &point,quint64 ts) {return point.tsUs<ts;});
        };
        for (int ch=first;ch<=last;++ch) {
            const auto &points=(*values)[ch];
            for (auto it=firstPoint(points);it!=points.end() && it->tsUs<end;++it) {
                lo=qMin(lo,it->value);hi=qMax(hi,it->value);
            }
        }
        if (!std::isfinite(lo)) {p.drawText(r,Qt::AlignCenter,QStringLiteral("无有效样本"));return;}
        const double pad=qMax((hi-lo)*0.1,0.001); lo-=pad; hi+=pad;
        auto axisFont=p.font();axisFont.setPointSize(9);p.setFont(axisFont);
        p.drawText(QRectF(0,r.top(),74,20),Qt::AlignRight,QString::number(hi,'g',4));
        p.drawText(QRectF(0,r.bottom()-20,74,20),Qt::AlignRight,QString::number(lo,'g',4));
        const QColor colors[]={QColor("#58a6ff"),QColor("#3fb950"),QColor("#f2cc60")};
        p.save(); p.setClipRect(r.adjusted(-1,-1,1,1));
        for (int ch=first;ch<=last;++ch) {
            p.setPen(QPen(colors[ch-first],1));
            int pixel=-1; double low=0,high=0; QPointF previous; quint64 previousTs=0; bool havePrevious=false;
            auto draw=[&] {
                if (pixel<0) return;
                const double x=r.left()+pixel;
                const auto y=[&](double v) {return r.bottom()-r.height()*(v-lo)/(hi-lo);};
                p.drawLine(QPointF(x,y(low)),QPointF(x,y(high)));
            };
            const quint64 maxGap=ch==0?6000000:ch<3?2000000:500000;
            const auto &points=(*values)[ch];
            for (auto it=firstPoint(points);it!=points.end() && it->tsUs<end;++it) {
                const auto &point=*it;
                const int x=int(double(point.tsUs-start)*r.width()/double(end-start));
                const QPointF now(r.left()+x,r.bottom()-r.height()*(point.value-lo)/(hi-lo));
                if (x!=pixel) {
                    draw(); pixel=x; low=high=point.value;
                    if (havePrevious && point.tsUs-previousTs<=maxGap) p.drawLine(previous,now);
                    else p.drawPoint(now);
                } else {low=qMin(low,point.value);high=qMax(high,point.value);}
                previous=now; previousTs=point.tsUs; havePrevious=true;
            }
            draw();
        }
        p.restore();
        if (m_channel==3) {p.setPen(QColor("#8b949e"));p.drawText(width()-100,20,"X / Y / Z");}
    }
private:
    int m_channel;
};

CaptureDialog::CaptureDialog(const AppConfig &config,QWidget *parent):QDialog(parent),m_config(config) {
    setObjectName("captureDialog"); setWindowTitle(QStringLiteral("采集记录与故障波形回放"));
    setWindowModality(Qt::WindowModal); resize(1300,820);
    auto *root=new QVBoxLayout(this); auto *body=new QHBoxLayout;
    auto *left=new QVBoxLayout;
    m_filter=new QComboBox(this); m_filter->setObjectName("captureFilter");
    m_filter->addItems({QStringLiteral("异常波形"),QStringLiteral("全部采集记录"),QStringLiteral("故障清单"),QStringLiteral("兼容记录（MLKF）")});
    auto *refreshButton=new QPushButton(QStringLiteral("刷新清单"),this);
    m_list=new QListWidget(this);m_list->setObjectName("captureList");m_list->setMinimumWidth(250);m_list->setMaximumWidth(340);
    m_list->installEventFilter(this);
    left->addWidget(m_filter);left->addWidget(refreshButton);left->addWidget(m_list,1);body->addLayout(left);
    auto *right=new QVBoxLayout;
    m_info=new QLabel(this);m_info->setTextFormat(Qt::PlainText);m_info->setWordWrap(true);m_info->setObjectName("captureDetails");
    m_related=new QComboBox(this);m_related->setObjectName("captureRelated");
    right->addWidget(m_info);right->addWidget(m_related);
    auto *tabs=new QTabWidget(this);auto *plots=new QWidget(tabs);auto *grid=new QGridLayout(plots);
    for (int i=0;i<4;++i) {m_plots[i]=new CapturePlot(i,plots);grid->addWidget(m_plots[i],i/2,i%2);}
    tabs->addTab(plots,QStringLiteral("四传感器波形"));
    m_table=new QTableView(tabs);m_table->setObjectName("captureData");m_tableModel=new CaptureTable(this);
    m_table->setModel(m_tableModel);m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tabs->addTab(m_table,QStringLiteral("对齐数据"));right->addWidget(tabs,1);
    auto *controls=new QHBoxLayout;
    m_play=new QPushButton(QStringLiteral("播放"),this);m_play->setCheckable(true);m_play->setObjectName("capturePlay");
    m_position=new QSlider(Qt::Horizontal,this);m_position->setRange(0,10000);m_position->setObjectName("capturePosition");
    m_span=new QDoubleSpinBox(this);m_span->setRange(0.1,3600);m_span->setValue(10);m_span->setSuffix(" s");
    m_step=new QSpinBox(this);m_step->setRange(1,1000);m_step->setValue(1);m_step->setSuffix(" ms");m_step->setObjectName("captureCsvStep");
    controls->addWidget(m_play);controls->addWidget(m_position,1);controls->addWidget(new QLabel(QStringLiteral("显示区间"),this));
    controls->addWidget(m_span);controls->addWidget(new QLabel(QStringLiteral("CSV 间隔"),this));controls->addWidget(m_step);
    right->addLayout(controls);
    auto *actions=new QHBoxLayout;
    m_export=new QPushButton(QStringLiteral("解密导出 CSV"),this);m_export->setObjectName("captureExport");
    m_label=new QPushButton(QStringLiteral("标注当前区间"),this);m_label->setObjectName("captureLabel");
    auto *closeButton=new QPushButton(QStringLiteral("关闭"),this);
    actions->addWidget(m_label);actions->addWidget(m_export);actions->addStretch();actions->addWidget(closeButton);right->addLayout(actions);
    body->addLayout(right,1);root->addLayout(body,1);
    m_status=new QLabel(this);m_status->setTextFormat(Qt::PlainText);m_status->setWordWrap(true);m_status->setObjectName("captureStatus");root->addWidget(m_status);
    m_timer=new QTimer(this);m_timer->setInterval(100);
    connect(m_timer,&QTimer::timeout,this,[this] {
        if (!m_loaded) return;
        const auto span=m_capture.endUs-m_capture.startUs;
        const int advance=qMax(1,int(100000.0/span*10000));
        if (m_position->value()+advance>=10000) {m_position->setValue(10000);m_play->setChecked(false);}
        else m_position->setValue(m_position->value()+advance);
    });
    connect(m_play,&QPushButton::toggled,this,[this](bool on) {
        m_play->setText(on?QStringLiteral("暂停"):QStringLiteral("播放"));
        if (on) {if(m_position->value()==10000)m_position->setValue(0);m_timer->start();} else m_timer->stop();
    });
    connect(m_position,&QSlider::valueChanged,this,[this] {render();});
    connect(m_span,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this] {render();});
    connect(m_step,qOverload<int>(&QSpinBox::valueChanged),this,[this] {
        m_tableModel->replace(m_loaded?CaptureStore::rows(m_capture,quint64(m_step->value())*1000):QVector<CaptureRow>{});
    });
    connect(m_filter,qOverload<int>(&QComboBox::currentIndexChanged),this,[this] {refresh();});
    connect(refreshButton,&QPushButton::clicked,this,[this] {refresh();});
    connect(m_list,&QListWidget::itemClicked,this,[this] {selectItem();});
    connect(m_related,qOverload<int>(&QComboBox::activated),this,[this] {loadSelected();});
    connect(m_export,&QPushButton::clicked,this,&CaptureDialog::exportCsv);
    connect(m_label,&QPushButton::clicked,this,&CaptureDialog::labelSelection);
    connect(closeButton,&QPushButton::clicked,this,&QDialog::accept);
    connect(this,&QDialog::finished,this,[this] {m_play->setChecked(false);});
    auto *watcher=new QFileSystemWatcher(this);
    if (QDir(m_config.storageDir).exists()) watcher->addPath(m_config.storageDir);
    connect(watcher,&QFileSystemWatcher::directoryChanged,this,[this] {
        if (isVisible() && !m_play->isChecked()) refresh();
    });
    clearRecord();refresh();
}
bool CaptureDialog::eventFilter(QObject *object,QEvent *event) {
    if (object==m_list && event->type()==QEvent::KeyPress) {
        const int key=static_cast<QKeyEvent *>(event)->key();
        if (key==Qt::Key_Return || key==Qt::Key_Enter) {selectItem();return true;}
    }
    return QDialog::eventFilter(object,event);
}
void CaptureDialog::configure(const AppConfig &config) {
    if (m_config.storageDir!=config.storageDir || m_config.passphrase!=config.passphrase) clearRecord();
    m_config=config;refresh();
}
void CaptureDialog::message(const QString &text) {m_status->setText(text);}
void CaptureDialog::refresh() {
    const QString selected=m_list->currentItem()?m_list->currentItem()->data(Qt::UserRole).toString():QString();
    const int scroll=m_list->verticalScrollBar()->value();
    CaptureStore store(m_config.storageDir,m_config.passphrase);QString error;
    if (!store.labels(m_labels,&error)) {message(error);return;}
    m_entries=store.list();
    QSignalBlocker blocker(m_list);m_list->clear();
    const int filter=m_filter->currentIndex();
    if (filter==2) {
        QHash<QString,QJsonObject> byId;
        for (const auto &label:m_labels) byId.insert(label.toObject().value("id").toString(),label.toObject());
        for (const auto &file:QDir(QDir(m_config.storageDir).filePath("orders")).entryInfoList({"*.mlko"},QDir::Files,QDir::Time)) {
            const auto label=byId.value(file.completeBaseName());
            QString title=QStringLiteral("工单 · %1").arg(file.completeBaseName());
            if (!label.isEmpty()) title=captureLabelText(label)+QStringLiteral("\n")+title;
            auto *item=new QListWidgetItem(title,m_list);
            item->setData(Qt::UserRole,file.absoluteFilePath());item->setData(Qt::UserRole+1,"order");
        }
    } else if (filter==3) {
        for (const auto &path:FaultStore(m_config.storageDir,true,m_config.passphrase).list()) {
            auto *item=new QListWidgetItem(QFileInfo(path).fileName(),m_list);
            item->setData(Qt::UserRole,path);item->setData(Qt::UserRole+1,"legacy");
        }
    } else for (const auto &entry:m_entries) {
        const auto labels=CaptureStore::matchingLabels(m_labels,entry.startUs,entry.endUs,true);
        if (filter==0 && labels.isEmpty()) continue;
        const auto time=QDateTime::fromMSecsSinceEpoch(qint64(entry.startUs/1000)).toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
        auto *item=new QListWidgetItem(QStringLiteral("%1 · %2 s\n%3").arg(time).arg(double(entry.endUs-entry.startUs)/1e6,0,'f',2)
            .arg(labels.isEmpty()?QStringLiteral("未标异常"):captureLabelText(labels.first().toObject())),m_list);
        item->setData(Qt::UserRole,entry.path);item->setData(Qt::UserRole+1,"capture");
    }
    int row=-1;bool openItemExists=false;
    for (int i=0;i<m_list->count();++i) {
        const auto path=m_list->item(i)->data(Qt::UserRole).toString();
        if (path==selected) row=i;
        if (path==m_openItemPath) openItemExists=true;
    }
    m_list->setCurrentRow(row);m_list->verticalScrollBar()->setValue(scroll);
    // Refreshing filenames and labels must not decrypt/rebuild the active
    // waveform or reset playback. Only an explicit activation loads data.
    if (!m_openItemPath.isEmpty() && !openItemExists) clearRecord();
    if (m_loaded) {updateCaptureDetails();render();}
    if (m_list->count() && m_openItemPath.isEmpty()) message(QStringLiteral("点击记录加载波形与数据，或选中后按回车。"));
    if (!m_list->count()) message(filter==0?QStringLiteral("暂无异常记录；模型 API 或人工标注后，重叠区间自动列入此清单。可切换到全部采集记录。"):
        QStringLiteral("当前清单为空"));
}
void CaptureDialog::clearRecord() {
    m_play->setChecked(false);m_loaded=false;m_eventUs=0;m_series={};m_tableModel->replace({});
    m_capture={};m_openItemPath.clear();m_loadedCapturePath.clear();
    m_export->setEnabled(false);m_label->setEnabled(false);m_play->setEnabled(false);m_info->clear();
    {QSignalBlocker blocker(m_related);m_related->clear();}
    for (auto *plot:m_plots) {plot->values=nullptr;plot->update();}
}
void CaptureDialog::selectItem() {
    const auto *item=m_list->currentItem();if (!item) return;
    const auto path=item->data(Qt::UserRole).toString(),kind=item->data(Qt::UserRole+1).toString();
    if (m_loaded && path==m_openItemPath) return;
    clearRecord();m_openItemPath=path;
    if (kind=="legacy") {openLegacy(path);return;}
    if (kind=="order") {
        QJsonObject order;QString error;
        if (!WorkOrders(m_config.storageDir,m_config.passphrase).load(QFileInfo(path).completeBaseName(),order,&error)) {message(error);return;}
        m_eventUs=order.value("faultTimeUs").toString().toULongLong();
        // v2 工单显示具体故障与等级；无 diagnosis 的历史工单只显示兼容粗分类，
        // 不从 message 推断类型。
        const int classIndex=order.value("diagnosis").toObject().value("classIndex").toInt(mlk::kFaultUnavailable);
        const bool specific=order.value("schema").toString()==QLatin1String(mlk::kFaultOrderSchema)
            && classIndex>=mlk::kFaultOverload && classIndex<=mlk::kFaultMountTilt;
        const auto detail=specific
            ? QStringLiteral("故障 %1 / 分级 %2").arg(mlk::faultDisplayName(classIndex))
                .arg(order.value("grade").toInt())
            : QStringLiteral("分类 %1 / 分级 %2").arg(order.value("class").toInt())
                .arg(order.value("grade").toInt());
        m_info->setText(QStringLiteral("工单 %1 · UTC %2 · %3\n%4")
            .arg(order.value("id").toString(),QString::number(m_eventUs),detail,
                 order.value("message").toString()));
        {QSignalBlocker blocker(m_related);
        const auto context=faultCaptureLabel(order);
        const quint64 begin=context.value("startUs").toString().toULongLong();
        const quint64 end=context.value("endUs").toString().toULongLong();
        for (const auto &entry:m_entries) if (entry.startUs<end && entry.endUs>begin)
            m_related->addItem(QFileInfo(entry.path).fileName(),entry.path);}
        if (!m_related->count()) {message(QStringLiteral("无关联采集记录：故障时间不在本机保存的记录区间内。"));return;}
    } else {QSignalBlocker blocker(m_related);m_related->addItem(QFileInfo(path).fileName(),path);}
    loadSelected();
}
void CaptureDialog::loadSelected() {
    const auto path=m_related->currentData().toString();
    if (m_loaded && path==m_loadedCapturePath) return;
    m_loaded=false;m_series={};m_tableModel->replace({});
    m_loadedCapturePath.clear();
    m_export->setEnabled(false);m_label->setEnabled(false);m_play->setEnabled(false);
    for (auto *plot:m_plots) {plot->values=nullptr;plot->update();}
    if (path.isEmpty()) return;
    QString error;Capture c;
    if (!CaptureStore(m_config.storageDir,m_config.passphrase).load(path,c,&error)) {message(error);return;}
    m_capture=std::move(c);m_series=CaptureStore::series(m_capture);m_loaded=true;m_loadedCapturePath=path;
    m_tableModel->replace(CaptureStore::rows(m_capture,quint64(m_step->value())*1000));
    m_export->setEnabled(true);m_label->setEnabled(true);m_play->setEnabled(true);m_position->setValue(0);
    if (m_eventUs && m_capture.endUs>m_capture.startUs) {
        const auto width=qMin(m_capture.endUs-m_capture.startUs,quint64(m_span->value()*1e6));
        const auto target=m_eventUs>width?m_eventUs-width:0;
        const auto travel=m_capture.endUs-m_capture.startUs-width;
        if (travel && target>m_capture.startUs)
            m_position->setValue(int(qMin(1.0,double(target-m_capture.startUs)/double(travel))*10000));
    }
    updateCaptureDetails();
    message(QStringLiteral("UTC 时间为估算的首样本时间；曲线共用时间轴。数据表与 CSV 使用前值保持，缺失或过期值显示不可用。"));
    render();
}
void CaptureDialog::updateCaptureDetails() {
    if (m_loaded && !m_eventUs) {
        const auto labels=CaptureStore::matchingLabels(m_labels,m_capture.startUs,m_capture.endUs);
        QStringList names;for (const auto &v:labels) names<<captureLabelText(v.toObject());
        m_info->setText(QStringLiteral("%1 · %2 · %3 个原始标量样本\n标签：%4")
            .arg(m_capture.id,m_capture.source).arg(m_capture.sampleCount()).arg(names.join(", ")));
    }
}
void CaptureDialog::render() {
    if (!m_loaded) return;
    const auto duration=m_capture.endUs-m_capture.startUs;
    const auto width=qMin(duration,quint64(m_span->value()*1e6));
    const auto start=m_capture.startUs+quint64(double(duration-width)*m_position->value()/10000);
    for (auto *plot:m_plots) {
        plot->values=&m_series;plot->origin=m_capture.startUs;plot->start=start;plot->end=start+width;plot->update();
        plot->labels=CaptureStore::matchingLabels(m_labels,start,start+width);
    }
    const auto index=int((start-m_capture.startUs)/(quint64(m_step->value())*1000));
    if (index<m_tableModel->rowCount()) m_table->scrollTo(m_tableModel->index(index,0),QAbstractItemView::PositionAtTop);
}
void CaptureDialog::exportCsv() {
    if (!m_loaded) return;
    const auto path=QFileDialog::getSaveFileName(this,QStringLiteral("导出解密后的 CSV"),
        QDir(m_config.storageDir).filePath(m_capture.id+".csv"),"CSV (*.csv)");
    if (path.isEmpty()) return;
    QString error;
    if (!CaptureStore(m_config.storageDir,m_config.passphrase).exportCsv(m_capture,path,quint64(m_step->value())*1000,&error)) message(error);
    else message(QStringLiteral("已导出明文 CSV：%1").arg(path));
}
void CaptureDialog::labelSelection() {
    if (!m_loaded) return;
    QDialog dialog(this);dialog.setWindowTitle(QStringLiteral("标注当前波形区间"));dialog.setWindowModality(Qt::WindowModal);
    auto *layout=new QFormLayout(&dialog);auto *name=new QLineEdit(&dialog);auto *author=new QLineEdit("manual",&dialog);
    auto *note=new QLineEdit(&dialog);auto *abnormal=new QCheckBox(QStringLiteral("列入异常波形清单"),&dialog);abnormal->setChecked(true);
    layout->addRow(QStringLiteral("标签"),name);layout->addRow(QStringLiteral("标注来源"),author);layout->addRow(QStringLiteral("说明"),note);layout->addRow(abnormal);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);layout->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec()!=QDialog::Accepted) return;
    const auto start=m_plots[0]->start,end=m_plots[0]->end;
    const QJsonObject label{{"api",1},{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"startUs",QString::number(start)},{"endUs",QString::number(end)},{"abnormal",abnormal->isChecked()},
        {"label",name->text().trimmed()},{"author",author->text()},{"note",note->text()}};
    QString error;
    if (!CaptureStore(m_config.storageDir,m_config.passphrase).putLabel(label,&error)) {message(error);return;}
    refresh();message(QStringLiteral("区间标签已加密保存"));
}
void CaptureDialog::openOrder(const QString &id) {
    {QSignalBlocker blocker(m_filter);m_filter->setCurrentIndex(2);}refresh();
    for (int i=0;i<m_list->count();++i) if (QFileInfo(m_list->item(i)->data(Qt::UserRole).toString()).completeBaseName()==id)
        {m_list->setCurrentRow(i);selectItem();return;}
    clearRecord();message(QStringLiteral("指定工单不在当前清单中"));
}
void CaptureDialog::openCapture(const QString &path) {
    {QSignalBlocker blocker(m_filter);m_filter->setCurrentIndex(1);}refresh();
    for (int i=0;i<m_list->count();++i) if (QFileInfo(m_list->item(i)->data(Qt::UserRole).toString())==QFileInfo(path))
        {m_list->setCurrentRow(i);selectItem();return;}
    clearRecord();message(QStringLiteral("指定采集记录不在当前清单中"));
}
void CaptureDialog::openLegacy(const QString &path) {
    FaultMeta meta;QVector<qint16> ch[4];QString error;
    if (!FaultStore(m_config.storageDir,true,m_config.passphrase).load(path,meta,ch,&error)) {message(error);return;}
    // MLKF v1 carries no channel schema. Do not reinterpret it as a complete
    // four-sensor capture or offer a misleading seven-column training export.
    auto *dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(QStringLiteral("MLKF 原始数据"));
    dialog->resize(700,500);auto *layout=new QVBoxLayout(dialog);
    auto *info=new QLabel(QStringLiteral("MLKF v1 无通道映射与逐样本时间；CH0–CH3 按文件原值显示，不能恢复遗漏的电流或转速。"),dialog);
    info->setWordWrap(true);layout->addWidget(info);auto *table=new QTableWidget(dialog);table->setColumnCount(4);
    table->setHorizontalHeaderLabels({"CH0","CH1","CH2","CH3"});table->setRowCount(qMin(10000,int(meta.n)));
    for (int c=0;c<4;++c) for (int i=0;i<qMin(10000,ch[c].size());++i) table->setItem(i,c,new QTableWidgetItem(QString::number(ch[c][i])));
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);layout->addWidget(table);dialog->show();
    message(QStringLiteral("兼容记录原始码预览已打开（最多 10000 行）；完整四传感器回放使用 MLKC 格式。"));
}
}
