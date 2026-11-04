#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QTimer>
#include <QSet>
#include <cstdio>
#include "frame.h"
#include "securechannel.h"
#include "sharedconfig.h"
#include "workorders.h"
int main(int argc,char **argv) {
 QCoreApplication a(argc,argv);if(argc<3)return 2;QFile file(argv[1]);if(!file.open(QIODevice::ReadOnly))return 3;
 auto cfg=QJsonDocument::fromJson(file.readAll()).object();const int mode=QString(argv[2]).toInt(); const int duration=argc>3?qBound(5,QString(argv[3]).toInt(),3600):20;
 mlk::SecureChannel channel,wrong; bool checkPin=mode==2,pinRejected=false;
 auto hex=[&](const char *n){return QByteArray::fromHex(cfg.value(n).toString().toLatin1());};
 channel.configure(mode==2?mlk::SecureChannel::Ecdh:mlk::SecureChannel::Psk,hex("psk"),hex("selfPub"),hex("selfPriv"),mode==2?mlk::crypto::randomBytes(32):hex("peerPub"));
 wrong.configure(mlk::SecureChannel::Psk,QByteArray(32,'x'),{},{},{});
 QUdpSocket socket;if(!socket.bind(QHostAddress(cfg.value("host").toString()),8000))return 4;
 QHostAddress board(cfg.value("board").toString());int types[8]={},samples[4]={},bad=0,preHandshake=0,plain=0,datagrams=0,once=0,forbidden=0,orders=0;bool snapshot=false,denied=false,started=false;
 QSet<quint64> seqs;quint64 lowest=~quint64(0),highest=0,bytes=0;QByteArray remoteSession;
 auto send=[&](const QByteArray &d){return socket.writeDatagram(d,board,8000);};
 auto query=[&](const QString &id){return mlk::buildObjectFrame(mlk::kTypeConfig,{{"op","get"},{"request",id}});};
 auto start=[&]{
  if(started)return;
  started=true;
  send(query("plaintext")); send(wrong.seal(query("wrong-key")));
  auto corrupt=channel.seal(query("tampered"));corrupt[corrupt.size()-1]=char(corrupt.back()^1);send(corrupt);
  auto duplicate=channel.seal(query("once"));send(duplicate);send(duplicate);
 };
 QObject::connect(&socket,&QUdpSocket::readyRead,&a,[&]{while(socket.hasPendingDatagrams()){
  auto dg=socket.receiveDatagram();if(dg.senderAddress()!=board||dg.senderPort()!=8000)continue;
  auto d=dg.data();++datagrams;bytes+=d.size();
  if(!mlk::SecureChannel::isSecureDatagram(d)){++plain;continue;}
  if(quint8(d[3])==2){if(checkPin){checkPin=false;pinRejected=!channel.handleReply(d);channel.configure(mlk::SecureChannel::Ecdh,{},hex("selfPub"),hex("selfPriv"),hex("peerPub"));send(channel.buildHello());continue;}if(!channel.handleReply(d)){++bad;continue;}start();continue;}
  // The intentional wrong-pin handshake can leave unauthenticated ciphertext
  // queued before the subsequent valid reply. It is outside the data session.
  if(!channel.ready()){++preHandshake;continue;}
  QByteArray p;if(!channel.open(d,p)){++bad;continue;}
  quint64 seq=0;for(int i=0;i<8;++i)seq|=quint64(quint8(d[20+i]))<<(8*i);
  auto session=d.mid(4,16);if(remoteSession.isEmpty())remoteSession=session;if(session==remoteSession){seqs.insert(seq);lowest=qMin(lowest,seq);highest=qMax(highest,seq);}
  mlk::Frame f;if(!mlk::parseFrame(p,f)){++bad;continue;}++types[f.type];
  if(f.type==mlk::kTypeWaveform){samples[f.ch]+=f.samples.size();if(f.ch==2&&(f.samplesY.size()!=f.samples.size()||f.samplesZ.size()!=f.samples.size()))++bad;}
  if(f.type==mlk::kTypeConfig){const auto id=f.object.value("request").toString();if(id=="once")++once;if(id=="plaintext"||id=="wrong-key"||id=="tampered")++forbidden;mlk::AppConfig c;snapshot|=mlk::readSharedConfig(f.object,c);if(id=="deny-write")denied=!f.object.value("error").toString().isEmpty();if(id=="tick"&&types[4]==2){auto o=f.object;o.insert("op","set");o.insert("request","deny-write");send(channel.seal(mlk::buildObjectFrame(4,o)));}}
  if(f.type==mlk::kTypeWorkOrder){if(mlk::WorkOrders("/tmp/mlk-hw-records","pc-test-password").save(f.object)){++orders;send(channel.seal(mlk::buildObjectFrame(6,{{"id",f.object.value("id")},{"status","stored"}})));}}
 }});
 QTimer timer;timer.setInterval(1000);QObject::connect(&timer,&QTimer::timeout,&a,[&]{if(channel.ready()){start();send(channel.seal(query("tick")));}else send(channel.buildHello());});timer.start();
 if(mode==1)start();else send(channel.buildHello());
 QTimer::singleShot(duration*1000,&a,[&]{
  QJsonObject o{{"mode",mode==2?"ECDH":"PSK"},{"durationSeconds",duration},{"wrongPinRejected",pinRejected},{"preHandshakeCiphertext",preHandshake},{"datagrams",datagrams},{"wireBytes",double(bytes)},{"badDecryptOrFrame",bad},{"plaintextReceived",plain},{"onceReplies",once},{"forbiddenReplies",forbidden},{"validConfig",snapshot},{"ownerWriteRejected",denied},{"ordersSaved",orders},{"sequenceGaps",double(highest>=lowest?highest-lowest+1-quint64(seqs.size()):0)}};
  QJsonObject counts;for(int i=0;i<8;++i)counts.insert(QString::number(i),types[i]);o.insert("frameTypes",counts);QJsonObject ch;for(int i=0;i<4;++i)ch.insert(QString::number(i),samples[i]);o.insert("samples",ch);
  bool ok=(mode!=2||pinRejected)&&snapshot&&denied&&once==1&&forbidden==0&&plain==0&&bad==0&&types[0]>0&&types[7]>0;
  o.insert("ok",ok);puts(QJsonDocument(o).toJson().constData());a.exit(ok?0:1);
 });return a.exec();
}
