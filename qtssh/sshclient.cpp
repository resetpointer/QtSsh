#include "sshclient.h"
#include <QTemporaryFile>
#include <QDir>
#include <QThread>
#include <QEventLoop>
#include "sshtunnelin.h"
#include "sshtunneloutsrv.h"
#include "sshprocess.h"
#include "sshscpsend.h"
#include "sshsftp.h"

static ssize_t qt_callback_libssh_recv(int socket,void *buffer, size_t length,int flags, void **abstract)
{
    Q_UNUSED(socket);
    Q_UNUSED(flags);

    QTcpSocket * c = reinterpret_cast<QTcpSocket *>(* abstract);
    int r = c->read(reinterpret_cast<char *>(buffer), length);
    if (r == 0)
    {
        return -EAGAIN;
    }
    return r;
}

static ssize_t qt_callback_libssh_send(int socket,const void * buffer, size_t length,int flags, void ** abstract)
{
    Q_UNUSED(socket);
    Q_UNUSED(flags);

    QTcpSocket * c = reinterpret_cast<QTcpSocket *>(* abstract);
    int r = c->write(reinterpret_cast<const char *>(buffer), length);
    if (r == 0)
    {
        return -EAGAIN;
    }
    return r;
}

bool SshClient::getSshConnected() const
{
    return _sshConnected;
}

SshClient::SshClient(QString name, QObject * parent):
    QObject(parent),
    _name(name),
    _session(NULL),
    _knownHosts(0),
    _sftp(NULL),
    _socket(this),
    _state(NoState),
    _errorcode(0),
    _sshConnected(false),
    _errorMessage(QString()),
    _cntTxData(0),
    _cntRxData(0)
{
#if defined(DEBUG_SSHCLIENT) || defined(DEBUG_THREAD)
    qDebug() << "DEBUG : SshClient("<< _name << ") : Enter in constructor, @" << this << " in " << QThread::currentThread() << " (" << QThread::currentThreadId() << ")";
#endif

    connect(&_socket,   SIGNAL(connected()),                         this, SLOT(_setStateConnected()));
    connect(&_socket,   SIGNAL(disconnected()),                      this, SLOT(_disconnected()));
    connect(&_socket,   SIGNAL(readyRead()),                         this, SLOT(_readyRead()));
    connect(&_socket,   SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(_tcperror(QAbstractSocket::SocketError)));
    connect(&_cntTimer, SIGNAL(timeout()),                           this, SLOT(_cntRate()));
    connect(&_keepalive,SIGNAL(timeout()),                           this, SLOT(_sendKeepAlive()));

    Q_ASSERT(libssh2_init(0) == 0);
    _session = libssh2_session_init_ex(NULL, NULL, NULL,reinterpret_cast<void *>(&_socket));

    libssh2_session_callback_set(_session, LIBSSH2_CALLBACK_RECV,reinterpret_cast<void*>(& qt_callback_libssh_recv));
    libssh2_session_callback_set(_session, LIBSSH2_CALLBACK_SEND,reinterpret_cast<void*>(& qt_callback_libssh_send));
    Q_ASSERT(_session);
    _knownHosts = libssh2_knownhost_init(_session);
    Q_ASSERT(_knownHosts);
    libssh2_session_set_blocking(_session, 0);

    _cntTimer.setInterval(1000);
    _cntTimer.start();
}

SshClient::~SshClient()
{
#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient("<< _name << ") : Enter in destructor, @"<< this << " state is " << _state;
#endif

    disconnectFromHost();

#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient("<< _name << ") : Quit destructor";
#endif
}


LIBSSH2_SESSION *SshClient::session()
{
    return _session;
}

bool SshClient::channelReady()
{
    return (_state == ActivatingChannels);
}

bool SshClient::waitForBytesWritten(int msecs)
{
    return _socket.waitForBytesWritten(msecs);
}

quint16 SshClient::openLocalPortForwarding(QString servicename, quint16 port, quint16 bind)
{
    if(_channels.contains(servicename))
    {
        return _channels.value(servicename)->localPort();
    }

    SshChannel *tunnel = qobject_cast<SshChannel *>(new SshTunnelIn(this, servicename, port, bind));
    _channels.insert(servicename, tunnel);
    return tunnel->localPort();
}

quint16 SshClient::openRemotePortForwarding(QString servicename, quint16 port)
{
    if(_channels.contains(servicename))
    {
        return _channels.value(servicename)->localPort();
    }

    SshChannel *tunnel = qobject_cast<SshChannel *>(new SshTunnelOutSrv(this, servicename, port));
    _channels.insert(servicename, tunnel);
    return tunnel->localPort();
}

void SshClient::closePortForwarding(QString servicename)
{
    if(_channels.contains(servicename))
    {
        SshChannel *tunnel = _channels.value(servicename);
        _channels.remove(servicename);
        delete tunnel;
    }
}

QString SshClient::runCommand(QString command)
{
    QString res;
    SshProcess *sshProcess = NULL;
    sshProcess = new SshProcess(this);
    if(sshProcess != NULL)
    {
        sshProcess->start(command);
        res = sshProcess->result();
    }
    return res;
}

QString SshClient::sendFile(QString src, QString dst)
{
    QEventLoop wait;
    SshScpSend *sender = new SshScpSend(this, src, dst);
    connect(sender, SIGNAL(transfertTerminate()), &wait, SLOT(quit()));
    QString d = sender->send();
    wait.exec();
#ifdef DEBUG_SSHCLIENT
    qDebug() << "DEBUG : Transfert file satus: " << sender->state();
#endif
    sender->deleteLater();
    return d;
}

void SshClient::enableSFTP()
{
    if(!_sftp)
    {
        _sftp = new SshSFtp(this);
#if defined(DEBUG_SSHCLIENT)
        qDebug() << "DEBUG : Enable sFtp";
#endif
        QObject::connect(_sftp, &SshSFtp::xfer, this, [this](){
            emit sFtpXfer();
        });
    }
}

QString SshClient::send(QString source, QString dest)
{
    QString res;
#if defined(DEBUG_SFTP)
    qDebug() << "DEBUG : SshClient::sFtpSend(" << source << "," << dest << ")";
#endif
    enableSFTP();
    res = _sftp->send(source, dest);
    return res;
}

bool SshClient::get(QString source, QString dest, bool override)
{
    bool res;
    enableSFTP();
    res = _sftp->get(source, dest, override);
    return res;
}

int SshClient::mkdir(QString dest)
{
    int res;
    enableSFTP();
    res = _sftp->mkdir(dest);
    qDebug() << "DEBUG : SshClient::sFtpMkdir " << dest << " = " << res;
    return res;
}

QStringList SshClient::readdir(QString d)
{
    QStringList res;
    enableSFTP();
    res = _sftp->readdir(d);
    return res;
}

bool SshClient::isDir(QString d)
{
    bool res;
    enableSFTP();
    res = _sftp->isDir(d);
    return res;
}

bool SshClient::isFile(QString d)
{
    bool res;
    enableSFTP();
    res = _sftp->isFile(d);
    return res;
}

int SshClient::mkpath(QString dest)
{
    int res;
    enableSFTP();
    res = _sftp->mkpath(dest);
    return res;
}

bool SshClient::unlink(QString d)
{
    bool res;
    enableSFTP();
    res = _sftp->unlink(d);
    return res;
}

quint64 SshClient::filesize(QString d)
{
    quint64 res;
    enableSFTP();
    res = _sftp->filesize(d);
    return res;
}

int SshClient::connectToHost(const QString & user, const QString & host, quint16 port, bool lock, bool checkHostKey, unsigned int retry )
{
    if(_sshConnected) {
        qDebug() << "ERROR : Allways connected";
        return 0;
    }
    bool failed = false;
    QEventLoop wait;
    QTimer timeout;
    _hostname = host;
    _username = user;
    _port = port;
    _state = TcpHostConnected;
    _errorcode = 0;

#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient("<< _name << ") : trying to connect to host (" << _hostname << ":" << _port << ")";
#endif

    timeout.setInterval(60*1000);
    auto c1 = connect(this, SIGNAL(_connectionTerminate()), &wait, SLOT(quit()));
    auto c2 = connect(&_socket, static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError err)>(&QAbstractSocket::error), [this, &wait, &failed](QAbstractSocket::SocketError err){
        if(err == QAbstractSocket::RemoteHostClosedError)
        {
            _errorcode = QAbstractSocket::RemoteHostClosedError;
            failed = true;
            wait.quit();
        }
    });
    auto c3 = connect(&timeout, &QTimer::timeout, [&wait, this](){
        _errorcode = LIBSSH2_ERROR_TIMEOUT;
        wait.quit();
    });
    timeout.start();
    do {
        _socket.connectToHost(_hostname, _port);
        if(lock)
        {
            wait.exec();
            if(_errorcode == LIBSSH2_ERROR_TIMEOUT) break;
            if(_errorcode == QAbstractSocket::RemoteHostClosedError) break;
        }
        /*
        if(!checkHostKey && _errorcode == HostKeyUnknownError)
        {
            SshKey serverKey = _hostKey;
            addKnownHost(_hostname, serverKey);
        }
        */
            Q_UNUSED(checkHostKey);
    }
    while(_errorcode && retry--);
    disconnect(c1);
    disconnect(c2);
    disconnect(c3);
    if(_errorcode || failed)
    {
        qDebug() << "WARNING : SshClient("<< _name << ") : SSH client NOT connected (" << _hostname << ":" << _port << " @" << user << ") -> " << _errorcode;
        if(failed) return -1;
        return _errorcode;
    }
#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient("<< _name << ") : SSH client connected (" << _hostname << ":" << _port << " @" << user << ")";
#endif

    _keepalive.setInterval(10000);
    _keepalive.start();
    libssh2_keepalive_config(_session, 1, 5);
    return _errorcode;
}

void SshClient::disconnectFromHost()
{
    if(!_sshConnected) return;

    QEventLoop wait;

    /* Close all Opened Channels */
    foreach(QString name, _channels.keys()){
        closePortForwarding(name);
    }

    /* Close SFTP if enabled */
    if(!_sftp) {
        delete _sftp;
        _sftp = NULL;
    }

    _keepalive.stop();
    emit sshReset();

    if (_knownHosts)
    {
        libssh2_knownhost_free(_knownHosts);
    }
    if (_state > TcpHostConnected)
    {
        libssh2_session_disconnect(_session, "good bye!");
    }
    if (_session)
    {
        libssh2_session_free(_session);
    }
    _state = NoState;
    _errorcode = 0;
    _errorMessage = QString();
    _failedMethods.clear();
    _availableMethods.clear();

    connect(this, SIGNAL(disconnected()), &wait, SLOT(quit()));
    _socket.disconnectFromHost();
    wait.exec();
    _socket.close();
}

void SshClient::setPassphrase(const QString & pass)
{
    _failedMethods.removeAll(SshClient::PasswordAuthentication);
    _failedMethods.removeAll(SshClient::PublicKeyAuthentication);
    _passphrase = pass;
    if(_state > TcpHostConnected)
    {
        QTimer::singleShot(0, this, SLOT(_readyRead()));
    }
}

void SshClient::setKeys(const QString &publicKey, const QString &privateKey)
{
    _failedMethods.removeAll(SshClient::PublicKeyAuthentication);
    _publicKey  = publicKey;
    _privateKey = privateKey;
    if(_state > TcpHostConnected)
    {
        QTimer::singleShot(0, this, SLOT(_readyRead()));
    }
}

bool SshClient::loadKnownHosts(const QString & file)
{
    bool res = (libssh2_knownhost_readfile(_knownHosts, qPrintable(file), LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0);
    return (res);
}

bool SshClient::saveKnownHosts(const QString & file)
{
    bool res = (libssh2_knownhost_writefile(_knownHosts, qPrintable(file), LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0);
    return res;
}

bool SshClient::addKnownHost(const QString & hostname,const SshKey & key)
{
    bool ret;
    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    switch (key.type)
    {
        case SshKey::Dss:
            typemask |= LIBSSH2_KNOWNHOST_KEY_SSHDSS;
            break;
        case SshKey::Rsa:
            typemask |= LIBSSH2_KNOWNHOST_KEY_SSHRSA;
            break;
        case SshKey::UnknownType:
            return false;
    };
    ret = (libssh2_knownhost_add(_knownHosts, qPrintable(hostname), NULL, key.key.data(), key.key.size(), typemask, NULL));
    return ret;
}

QString SshClient::banner()
{
    return QString(libssh2_session_banner_get(_session));
}


void SshClient::_setStateConnected()
{
#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient("<< _name << ") : ssh socket connected";
#endif
    _state = InitializeSession;
    _readyRead();
    _sshConnected = true;
}

void SshClient::_tcperror(QAbstractSocket::SocketError err)
{
    if(err == QAbstractSocket::ConnectionRefusedError)
    {
        _errorcode = LIBSSH2_ERROR_BAD_SOCKET;
        qDebug() << "ERROR : SshClient("<< _name << ") : ConnectionRefusedError";
        emit _connectionTerminate();
    }
    else
    {
        qDebug() << "ERROR : SshClient("<< _name << ") : failed to connect session tcp socket, err=" << err;
    }
}

void SshClient::tx_data(qint64 len)
{
    _cntTxData += len;
}

void SshClient::rx_data(qint64 len)
{
    _cntRxData += len;
}

void SshClient::_cntRate()
{
    emit xfer_rate(_cntTxData, _cntRxData);
    _cntRxData = 0;
    _cntTxData = 0;
}

void SshClient::_sendKeepAlive()
{
    int keepalive;
    libssh2_keepalive_send(_session, &keepalive);
}

void SshClient::_readyRead()
{
    switch (_state)
    {
        case InitializeSession:
        {
            int sock = _socket.socketDescriptor();
            int ret = 0;

            if ((ret = libssh2_session_startup(_session, sock)) == LIBSSH2_ERROR_EAGAIN)
            {
                return;
            }
            if (ret)
            {
                qWarning() << "WARNING : SshClient("<< _name << ") : Failure establishing SSH session :" << ret;
                _getLastError();
                emit _connectionTerminate();
                askDisconnect();
                return;
            }
            size_t len;
            int type;
            const char * fingerprint = libssh2_session_hostkey(_session, &len, &type);
            _hostKey.key = QByteArray(fingerprint, len);
            _hostKey.hash = QByteArray(libssh2_hostkey_hash(_session,LIBSSH2_HOSTKEY_HASH_MD5), 16);
            switch (type)
            {
                case LIBSSH2_HOSTKEY_TYPE_RSA:
                {
                    _hostKey.type=SshKey::Rsa;
                    break;
                }
                case LIBSSH2_HOSTKEY_TYPE_DSS:
                {
                    _hostKey.type=SshKey::Dss;
                    break;
                }
                default:
                {
                    _hostKey.type=SshKey::UnknownType;
                }
            }
            if (fingerprint)
            {
                struct libssh2_knownhost *host;
                libssh2_knownhost_check(_knownHosts,
                                        qPrintable(_hostname),
                                        (char *)fingerprint,
                                        len,
                                        LIBSSH2_KNOWNHOST_TYPE_PLAIN|
                                        LIBSSH2_KNOWNHOST_KEYENC_RAW,
                                        &host);

               _state=RequestAuthTypes;
               _readyRead();
               return;
              }
            break;
        }
        case RequestAuthTypes:
        {
            QByteArray username = _username.toLocal8Bit();
            char * alist = libssh2_userauth_list(_session, username.data(), username.length());
            if (alist == NULL)
            {
                if (libssh2_userauth_authenticated(_session))
                {
                    //null auth ok
                    _setStateConnected();
                    _state = TryingAuthentication;
                    return;
                }
                else if (libssh2_session_last_error(_session, NULL, NULL, 0) == LIBSSH2_ERROR_EAGAIN)
                {
                    return;
                }
                else
                {
                    _getLastError();
                    qDebug() << "ERROR : UnexpectedShutdownError";
                    emit _connectionTerminate();
                    askDisconnect();
                    emit disconnected();
                    return;
                }
            }
            foreach (QByteArray m, QByteArray(alist).split(','))
            {
                if (m == "publickey")
                {
                    _availableMethods<<SshClient::PublicKeyAuthentication;
                }
                else if (m == "password")
                {
                    _availableMethods<<SshClient::PasswordAuthentication;
                }
            }
            _state = LookingAuthOptions;
            _readyRead();
            break;
        }
        case LookingAuthOptions:
        {
            if (_availableMethods.contains(SshClient::PublicKeyAuthentication) && !_privateKey.isNull() && (!_failedMethods.contains(SshClient::PublicKeyAuthentication) ))
            {
                _currentAuthTry = SshClient::PublicKeyAuthentication;
                _state = TryingAuthentication;
                _readyRead();
                return;
            }
            else if (_availableMethods.contains(SshClient::PasswordAuthentication) && !_passphrase.isNull() && (!_failedMethods.contains(SshClient::PasswordAuthentication)))
            {
                _currentAuthTry = SshClient::PasswordAuthentication;
                _state = TryingAuthentication;
                _readyRead();
                return;
            }
            _errorcode = LIBSSH2_ERROR_AUTHENTICATION_FAILED;
            emit _connectionTerminate();
            break;
        }
        case TryingAuthentication:
        {
            int ret = 0;
            if (_currentAuthTry == SshClient::PasswordAuthentication)
            {
                ret = libssh2_userauth_password(_session,
                                                qPrintable(_username),
                                                qPrintable(_passphrase));

            }
            else if (_currentAuthTry == SshClient::PublicKeyAuthentication)
            {
                QTemporaryFile key_public, key_private;

                key_public.open();
                key_public.write(this->_publicKey.toLatin1());
                key_public.close();
                key_private.open();
                key_private.write(this->_privateKey.toLatin1());
                key_private.close();

                ret=libssh2_userauth_publickey_fromfile(_session,
                                                        qPrintable(_username),
                                                        qPrintable(key_public.fileName()),
                                                        qPrintable(key_private.fileName()),
                                                        qPrintable(_passphrase));

            }
            if (ret == LIBSSH2_ERROR_EAGAIN)
            {
                return;
            }
            else if (ret == 0)
            {
                _state = ActivatingChannels;
                _errorcode = LIBSSH2_ERROR_NONE;
#if defined(DEBUG_SSHCLIENT) || defined(DEBUG_THREAD)
    qDebug() << "DEBUG : SshClient("<< _name << ") : Connected, @" << this << " in " << QThread::currentThread() << " (" << QThread::currentThreadId() << ")";
#endif
                emit connected();
                emit _connectionTerminate();
            }
            else
            {
                _getLastError();
                qDebug() << "ERROR : AuthenticationError";
                emit _connectionTerminate();
                _failedMethods.append(_currentAuthTry);
                _state = LookingAuthOptions;
                _readyRead();
            }
            break;
        }
        case ActivatingChannels:
        {
            emit sshDataReceived();
            break;
        }
        default :
        {
            qDebug() << "WARNING : SshClient : did not expect to receive data in this state =" << _state;
            break;
        }
    }
}

void SshClient::askDisconnect()
{
    /* Close all Opened Channels */
    foreach(QString name, _channels.keys()){
        closePortForwarding(name);
    }

#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient("<< _name << ") : reset";
#endif
    _keepalive.stop();
    emit sshReset();

    if (_knownHosts)
    {
        libssh2_knownhost_free(_knownHosts);
    }
    if (_state > TcpHostConnected)
    {
        libssh2_session_disconnect(_session, "good bye!");
    }
    if (_session)
    {
        libssh2_session_free(_session);
    }
    _state = NoState;
    _errorcode = 0;
    _errorMessage = QString();
    _failedMethods.clear();
    _availableMethods.clear();
    _session = libssh2_session_init_ex(NULL, NULL, NULL,reinterpret_cast<void *>(this));

    libssh2_session_callback_set(_session, LIBSSH2_CALLBACK_RECV,reinterpret_cast<void*>(& qt_callback_libssh_recv));
    libssh2_session_callback_set(_session, LIBSSH2_CALLBACK_SEND,reinterpret_cast<void*>(& qt_callback_libssh_send));
    Q_ASSERT(_session);
    _knownHosts = libssh2_knownhost_init(_session);
    Q_ASSERT(_knownHosts);
    libssh2_session_set_blocking(_session, 0);
    _socket.disconnectFromHost();
#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient("<< _name << ") : reset disconnected";
#endif
}

void SshClient::_disconnected()
{
    _keepalive.stop();
    if (_state != NoState)
    {
        qWarning() << "WARNING : SshClient("<< _name << ") : unexpected shutdown";
        emit unexpectedDisconnection();
        askDisconnect();
    }

    _sshConnected = false;
    emit disconnected();
}

void SshClient::_getLastError()
{
    char * msg;
    int len = 0;
    _errorcode = libssh2_session_last_error(_session, &msg, &len, 0);
    _errorMessage = QString::fromLocal8Bit(QByteArray::fromRawData(msg, len));
}

