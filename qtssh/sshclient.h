#ifndef QT_SSH_CLIENT_H
#define QT_SSH_CLIENT_H

#include <QObject>
#include <QList>
#include <QTcpSocket>
#include <QTimer>
#include "sshchannel.h"
#include "sshfsinterface.h"
#include "sshinterface.h"

extern "C" {
#include <libssh2.h>
#include <errno.h>
}

class SshSFtp;



class  SshClient : public QObject, public SshFsInterface, public SshInterface {
    Q_OBJECT

private:
    enum SshState {
        NoState = 0,
        TcpHostConnected = 1,
        InitializeSession = 2,
        RequestAuthTypes = 3,
        LookingAuthOptions = 4,
        TryingAuthentication = 5,
        ActivatingChannels = 6
    };

    enum AuthenticationMethod {
        PasswordAuthentication,
        PublicKeyAuthentication
    };


    QString _name;
    // libssh2 stuff
    LIBSSH2_SESSION    * _session;
    LIBSSH2_KNOWNHOSTS * _knownHosts;
    SshSFtp            *_sftp;
    QMap<QString,SshChannel*>   _channels;
    QTcpSocket _socket;

    SshState _state;

    int _port;
    int _errorcode;
    bool _sshConnected;

    QString _hostname;
    QString _username;
    QString _passphrase;
    QString _privateKey;
    QString _publicKey;

    QString _errorMessage;

    SshKey  _hostKey;

    // authentication methods
    SshClient::AuthenticationMethod        _currentAuthTry;
    QList<SshClient::AuthenticationMethod> _availableMethods;
    QList<SshClient::AuthenticationMethod> _failedMethods;

    qint64 _cntTxData;
    qint64 _cntRxData;
    QTimer _cntTimer;
    QTimer _keepalive;

public:
    SshClient(QString name = "noname", QObject * parent = NULL);
    virtual ~SshClient();

/* <<<SshInterface>>> */
public slots:
    int connectToHost(const QString & username, const QString & hostname, quint16 port = 22, bool lock = true, bool checkHostKey = false, unsigned int retry = 5);
    void disconnectFromHost();
    QString runCommand(QString command);
    quint16 openLocalPortForwarding(QString servicename, quint16 port, quint16 bind);
    quint16 openRemotePortForwarding(QString servicename, quint16 port);
    void closePortForwarding(QString servicename);
    void setKeys(const QString &publicKey, const QString &privateKey);
    bool loadKnownHosts(const QString &file);
    QString sendFile(QString src, QString dst);
    void setPassphrase(const QString & pass);
    bool saveKnownHosts(const QString &file);
    bool addKnownHost  (const QString &hostname, const SshKey &key);
    QString banner();
/* >>>SshInterface<<< */

/* <<<SshFsInterface>>> */
public slots:
    void enableSFTP();
    QString send(QString source, QString dest);
    bool get(QString source, QString dest, bool override = false);
    int mkdir(QString dest);
    QStringList readdir(QString d);
    bool isDir(QString d);
    bool isFile(QString d);
    int mkpath(QString dest);
    bool unlink(QString d);
    quint64 filesize(QString d);
/* >>>SshFsInterface<<< */


    LIBSSH2_SESSION *session();
    bool channelReady();
    bool waitForBytesWritten(int msecs);
    bool getSshConnected() const;


signals:
    void connected();
    void unexpectedDisconnection();
    void disconnected();
    void xfer_rate(qint64 tx, qint64 rx);
    void sshDataReceived();
    void sshReset();
    void _connectionTerminate();
    void sFtpXfer();


public slots:
    void tx_data(qint64 len);
    void rx_data(qint64 len);
    void askDisconnect();


private slots:
    void _readyRead();
    void _setStateConnected();
    void _disconnected();
    void _getLastError();
    void _tcperror(QAbstractSocket::SocketError err);
    void _cntRate();
    void _sendKeepAlive();
};

#endif
