#include "capturestore.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>
#include <QLocale>

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);QCoreApplication::setApplicationName("mlk-capture");
    QCommandLineParser parser;parser.setApplicationDescription("Encrypted MLKC acquisition archive and training CSV tool");
    parser.addHelpOption();
    parser.addOptions({{{"d","directory"},"Archive directory","directory"},
        {"password-file","UTF-8 password file; strip one trailing LF or CRLF","file"},
        {"list","List captures and abnormal labels as JSON"},{"labels","Print decrypted labels as JSON"},
        {"label-json","Upsert one label from a JSON file","file"},
        {"input","MLKC input file","file"},{"csv","Decrypted aligned CSV output","file"},
        {"native-csv","Decrypted native sample CSV output (no resampling)","file"},
        {"inspect","Print decrypted capture metadata"},
        {"step-us","Aligned CSV grid interval, 1000..1000000 microseconds","microseconds","1000"}});
    parser.process(app);QTextStream out(stdout),err(stderr);
    auto fail=[&](const QString &message) {err<<message<<'\n';return 1;};
    QFile passwordFile(parser.value("password-file"));
    if (!passwordFile.open(QIODevice::ReadOnly) || passwordFile.size()>4096)
        return fail("--password-file must name a readable UTF-8 file (maximum 4096 bytes)");
    auto password=passwordFile.readAll();
    if (password.endsWith('\n')) {password.chop(1);if(password.endsWith('\r')) password.chop(1);}
    if (password.isEmpty()) return fail("Empty password");
    mlk::AppConfig config;config.storageDir=parser.value("directory");config.passphrase=QString::fromUtf8(password);
    if (config.storageDir.isEmpty() && !parser.value("input").isEmpty()) config.storageDir=QFileInfo(parser.value("input")).absolutePath();
    if (config.storageDir.isEmpty()) return fail("--directory or --input is required");
    mlk::CaptureStore store(config.storageDir,config.passphrase);QString error;
    if (parser.isSet("list") || parser.isSet("labels") || parser.isSet("label-json")) {
        QJsonObject request{{"api",1},{"op",parser.isSet("list")?"list":"labels"},{"limit",1000}};
        if (parser.isSet("label-json")) {
            QFile file(parser.value("label-json"));if (!file.open(QIODevice::ReadOnly) || file.size()>8192) return fail("Cannot read label JSON");
            QJsonParseError parseError;const auto doc=QJsonDocument::fromJson(file.readAll(),&parseError);
            if (!doc.isObject() || parseError.error!=QJsonParseError::NoError) return fail("Invalid label JSON");
            request={{"api",1},{"op","label"},{"label",doc.object()}};
        }
        const auto response=mlk::captureRequest(config,request);out<<QJsonDocument(response).toJson(QJsonDocument::Indented);
        return response.value("ok").toBool()?0:1;
    }
    mlk::Capture capture;
    if (!store.load(parser.value("input"),capture,&error)) return fail(error);
    if (parser.isSet("inspect")) {
        QJsonArray count;const auto series=mlk::CaptureStore::series(capture);
        for (const auto &values:series) count.append(values.size());
        out<<QJsonDocument(QJsonObject{{"id",capture.id},{"source",capture.source},
            {"startUs",QString::number(capture.startUs)},{"endUs",QString::number(capture.endUs)},
            {"scalarSamples",double(capture.sampleCount())},{"sampleCountsTempCurrentSpeedXYZ",count},
            {"timebase","estimated_utc_first_sample"}}).toJson(QJsonDocument::Indented);
    }
    if (parser.isSet("csv")) {
        bool ok=false;const auto step=parser.value("step-us").toULongLong(&ok);
        if (!ok || step<1000 || step>1000000) return fail("--step-us must be 1000..1000000");
        if (QFileInfo(parser.value("csv"))==QFileInfo(parser.value("input"))) return fail("Output must differ from input");
        if (!store.exportCsv(capture,parser.value("csv"),step,&error)) return fail(error);
    }
    if (parser.isSet("native-csv")) {
        if (QFileInfo(parser.value("native-csv"))==QFileInfo(parser.value("input"))) return fail("Output must differ from input");
        QSaveFile file(parser.value("native-csv"));if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
        file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
        QTextStream stream(&file);stream.setRealNumberPrecision(12);stream.setLocale(QLocale::c());
        stream<<"timestamp_us,channel,raw_x,raw_y,raw_z,value_x,value_y,value_z,rate_hz,seq,count\n";
        for (const auto &f:capture.blocks) for (int i=0;i<f.samples.size();++i) {
            const bool xyz=!f.samplesY.isEmpty();
            stream<<f.tsUs+quint64(i)*1000000/f.rateHz<<','<<int(f.ch)<<','<<f.samples[i]<<',';
            if (xyz) stream<<f.samplesY[i];
            stream<<',';
            if (xyz) stream<<f.samplesZ[i];
            stream<<','<<f.samples[i]*capture.k[f.ch]+capture.b[f.ch]<<',';
            if (xyz) stream<<f.samplesY[i]*capture.k[f.ch]+capture.b[f.ch];
            stream<<',';
            if (xyz) stream<<f.samplesZ[i]*capture.k[f.ch]+capture.b[f.ch];
            stream<<','<<f.rateHz<<','<<f.seq<<','<<f.cnt<<'\n';
        }
        stream.flush();if(stream.status()!=QTextStream::Ok || !file.commit()) return fail(file.errorString());
    }
    if (!parser.isSet("csv") && !parser.isSet("native-csv") && !parser.isSet("inspect")) return fail("Choose --csv, --native-csv or --inspect");
    return 0;
}
