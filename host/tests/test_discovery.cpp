#include <QtTest>
#include <QNetworkDatagram>
#include <QtEndian>
#include "peerdiscovery.h"
Q_DECLARE_METATYPE(QHostAddress)

class DiscoveryTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<QHostAddress>(); }
    void authenticatedAddressChange() {
        mlk::SecureChannel pc, board;
        pc.configure(mlk::SecureChannel::Psk,QByteArray(32,'a'),{},{},{});
        board.configure(mlk::SecureChannel::Psk,QByteArray(32,'a'),{},{},{});
        mlk::PeerDiscovery client(mlk::PeerDiscovery::Remote,nullptr,19801,QHostAddress("127.0.0.2"));
        mlk::PeerDiscovery server(mlk::PeerDiscovery::Board,nullptr,19801,QHostAddress("127.0.0.3"));
        QVERIFY(client.configure(pc,8000,true,QHostAddress("127.0.0.9")));
        QVERIFY(server.configure(board,9000,true));
        QSignalSpy found(&client,&mlk::PeerDiscovery::peerDiscovered);
        QVERIFY(client.probe(QHostAddress("127.0.0.3"),QHostAddress("127.0.0.2")));
        QTRY_COMPARE(found.count(),1);
        QCOMPARE(qvariant_cast<QHostAddress>(found[0][0]),QHostAddress("127.0.0.3"));
        QCOMPARE(found[0][1].value<quint16>(),quint16(9000));
        QVERIFY(server.configure(board,9000,false));
        mlk::PeerDiscovery moved(mlk::PeerDiscovery::Board,nullptr,19801,QHostAddress("127.0.0.4"));
        QVERIFY(moved.configure(board,9000,true));
        QVERIFY(client.probe(QHostAddress("127.0.0.4"),QHostAddress("127.0.0.2")));
        QTRY_COMPARE(found.count(),2);
        QCOMPARE(qvariant_cast<QHostAddress>(found[1][0]),QHostAddress("127.0.0.4"));
        board.configure(mlk::SecureChannel::Psk,QByteArray(32,'b'),{},{},{});
        QVERIFY(moved.configure(board,9000,true));
        QVERIFY(client.probe(QHostAddress("127.0.0.4"),QHostAddress("127.0.0.2")));
        QTest::qWait(200); QCOMPARE(found.count(),2);
        mlk::SecureChannel off;
        QVERIFY(client.configure(off,8000,true)); QVERIFY(!client.isListening());
    }
    void ecdhBeforeHandshake() {
        QByteArray ap,as,bp,bs; QVERIFY(mlk::SecureChannel::generateIdentity(ap,as)); QVERIFY(mlk::SecureChannel::generateIdentity(bp,bs));
        mlk::SecureChannel a,b;
        a.configure(mlk::SecureChannel::Ecdh,{},ap,as,bp); b.configure(mlk::SecureChannel::Ecdh,{},bp,bs,ap);
        QVERIFY(!a.ready() && !b.ready()); QCOMPARE(a.discoveryKey(),b.discoveryKey());
        mlk::PeerDiscovery client(mlk::PeerDiscovery::Remote,nullptr,19802,QHostAddress("127.0.0.2"));
        mlk::PeerDiscovery server(mlk::PeerDiscovery::Board,nullptr,19802,QHostAddress("127.0.0.3"));
        QVERIFY(client.configure(a,8000,true)); QVERIFY(server.configure(b,8000,true));
        QSignalSpy found(&client,&mlk::PeerDiscovery::peerDiscovered);
        QVERIFY(client.probe(QHostAddress("127.0.0.3"),QHostAddress("127.0.0.2")));
        QTRY_COMPARE(found.count(),1);
        QVERIFY(!a.ready()); // Discovery does not establish an application session.
    }
    void tamperReplayAndEndpointBinding() {
        mlk::SecureChannel key; key.configure(mlk::SecureChannel::Psk,QByteArray(32,'c'),{},{},{});
        mlk::PeerDiscovery client(mlk::PeerDiscovery::Remote,nullptr,19803,QHostAddress("127.0.0.2"));
        QVERIFY(client.configure(key,8000,true));
        QUdpSocket responder; QVERIFY(responder.bind(QHostAddress("127.0.0.4"),19803));
        QSignalSpy found(&client,&mlk::PeerDiscovery::peerDiscovered);
        QVERIFY(client.probe(QHostAddress("127.0.0.4"),QHostAddress("127.0.0.2")));
        QTRY_VERIFY(responder.hasPendingDatagrams());
        auto header=responder.receiveDatagram().data().left(30);
        header[5]=char(2); header[6]=char(mlk::PeerDiscovery::Board);
        auto send=[&](const QByteArray &data){responder.writeDatagram(data,QHostAddress("127.0.0.2"),19803);};
        auto sign=[&]{return header+mlk::crypto::hmacSha256(key.discoveryKey(),header);};
        qToBigEndian<quint32>(QHostAddress("127.0.0.3").toIPv4Address(),reinterpret_cast<uchar *>(header.data()+26));
        send(sign()); QTest::qWait(80); QCOMPARE(found.count(),0);
        qToBigEndian<quint32>(QHostAddress("127.0.0.4").toIPv4Address(),reinterpret_cast<uchar *>(header.data()+26));
        auto bad=sign(); bad[61]=char(bad[61]^1); send(bad); QTest::qWait(80); QCOMPARE(found.count(),0);
        const auto valid=sign(); send(valid); QTRY_COMPARE(found.count(),1);
        send(valid); QTest::qWait(80); QCOMPARE(found.count(),1);
        QVERIFY(client.probe(QHostAddress("127.0.0.4"),QHostAddress("127.0.0.2")));
        QTRY_VERIFY(responder.hasPendingDatagrams());
        header=responder.receiveDatagram().data().left(30); header[5]=char(2); header[6]=char(mlk::PeerDiscovery::Board);
        qToBigEndian<quint32>(QHostAddress("127.0.0.4").toIPv4Address(),reinterpret_cast<uchar *>(header.data()+26));
        QTest::qWait(5200); send(sign()); QTest::qWait(80); QCOMPARE(found.count(),1);
    }
};
QTEST_GUILESS_MAIN(DiscoveryTest)
#include "test_discovery.moc"
