#include "sshclient.h"
#include <QTemporaryFile>
#include <QDir>
#include <QEventLoop>

SshClient::SshClient(QObject * parent): 
    QTcpSocket(parent),
    _session(NULL),
    _knownHosts(0),
    _state(NoState),
    _errorcode(0),
    _cntTxData(0),
    _cntRxData(0)
{
#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient : Enter in constructor, @" << this;
#endif

    connect(this,       SIGNAL(connected()),                         this, SLOT(_connected()));
    connect(this,       SIGNAL(disconnected()),                      this, SLOT(_disconnected()));
    connect(this,       SIGNAL(readyRead()),                         this, SLOT(_readyRead()));
    connect(this,       SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(_tcperror(QAbstractSocket::SocketError)));
    connect(&_cntTimer, SIGNAL(timeout()),                           this, SLOT(_cntRate()));
    connect(&_keepalive,SIGNAL(timeout()),                           this, SLOT(_sendKeepAlive()));

    Q_ASSERT(libssh2_init(0) == 0);
    _reset();
    _cntTimer.setInterval(1000);
    _cntTimer.start();
}

SshClient::~SshClient()
{
#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient : Enter in destructor, @"<< this << " state is " << _state;
#endif
    this->disconnect();

    if (_session)
    {
        libssh2_knownhost_free(_knownHosts);
        libssh2_session_free(_session);
    }
#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient : Quit destructor";
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


int SshClient::connectSshToHost(const QString & user, const QString & host, quint16 port, bool lock, bool checkHostKey, unsigned int retry )
{
    QEventLoop wait;
    QTimer timeout;
    _hostname = host;
    _username = user;
    _port = port;
    _state = TcpHostConnected;
    _errorcode = TimeOut;

#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient : trying to connect to host (" << _hostname << ":" << _port << ")";
#endif

    timeout.setInterval(10*1000);
    connect(this, SIGNAL(_connectionTerminate()), &wait, SLOT(quit()));
    connect(&timeout, SIGNAL(timeout()), &wait, SLOT(quit()));
    timeout.start();
    do {
        QTcpSocket::connectToHost(_hostname, _port);
        if(lock)
        {
            wait.exec();
            if(_errorcode == TimeOut) break;
        }
        if(!checkHostKey && _errorcode == HostKeyUnknownError)
        {
            SshKey serverKey = hostKey();
            addKnownHost(hostName(), serverKey);
        }
    }
    while(_errorcode && retry--);
#if defined(DEBUG_SSHCLIENT)
    qDebug() << "DEBUG : SshClient : SSH client connected (" << _hostname << ":" << _port << " @" << user << ")";
#endif

    _keepalive.setInterval(10000);
    _keepalive.start();
    libssh2_keepalive_config(_session, 1, 5);
    return _errorcode;
}

void SshClient::disconnectFromHost()
{
    _reset();
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

bool SshClient::loadKnownHosts(const QString & file, KnownHostsFormat c)
{
    Q_UNUSED(c);
    return (libssh2_knownhost_readfile(_knownHosts, qPrintable(file), LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0);
}

bool SshClient::saveKnownHosts(const QString & file, KnownHostsFormat c) const
{
    Q_UNUSED(c);
    return (libssh2_knownhost_writefile(_knownHosts, qPrintable(file), LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0);
}

bool SshClient::addKnownHost(const QString & hostname,const SshKey & key)
{
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

    return (libssh2_knownhost_add(_knownHosts, qPrintable(hostname), NULL, key.key.data(), key.key.size(), typemask, NULL));

}

quint16 SshClient::sshSocketLocalPort()
{
    return localPort();
}

SshKey SshClient::hostKey() const
{
    return _hostKey;
}

QString SshClient::hostName() const
{
    return _hostname;
}

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


void SshClient::_connected()
{
#if defined(DEBUG_SSHCLIENT)
    qDebug("DEBUG : SshClient : ssh socket connected");
#endif
    _state = InitializeSession;
    _readyRead();
}

void SshClient::_tcperror(QAbstractSocket::SocketError err)
{
    if(err == QAbstractSocket::ConnectionRefusedError)
    {
        _errorcode = ConnectionRefusedError;
        emit sshError(ConnectionRefusedError);
        emit _connectionTerminate();
    }
    else
    {
        qDebug() << "ERROR : SshClient : failed to connect session tcp socket, err=" << err;
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
            int sock=socketDescriptor();
            int ret = 0;

            if ((ret = libssh2_session_startup(_session, sock)) == LIBSSH2_ERROR_EAGAIN)
            {
                return;
            }
            if (ret)
            {
                qWarning() << "WARNING : SshClient : Failure establishing SSH session :" << ret;
                emit sshError(UnexpectedShutdownError);
                _errorcode = UnexpectedShutdownError;
                emit _connectionTerminate();
                _reset();
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
                    emit connected();
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
                    emit sshError(UnexpectedShutdownError);
                    _errorcode = UnexpectedShutdownError;
                    emit _connectionTerminate();
                    _reset();
                    emit sshDisconnected();
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
            if (_availableMethods.contains(SshClient::PublicKeyAuthentication) &&
                !_privateKey.isNull() &&
                !_failedMethods.contains(SshClient::PublicKeyAuthentication))
            {
                _currentAuthTry = SshClient::PublicKeyAuthentication;
                _state = TryingAuthentication;
                _readyRead();
                return;
            }
            if (_availableMethods.contains(SshClient::PasswordAuthentication) &&
                !_passphrase.isNull() &&
                !_failedMethods.contains(SshClient::PasswordAuthentication))
            {
                _currentAuthTry = SshClient::PasswordAuthentication;
                _state = TryingAuthentication;
                _readyRead();
                return;
            }
            _errorcode = AuthenticationError;
            emit sshAuthenticationRequired(_availableMethods);
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
                _errorcode = NoError;
                emit sshConnected();
                emit _connectionTerminate();
            }
            else
            {
                _getLastError();
                _errorcode = AuthenticationError;
                emit sshError(AuthenticationError);
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

void SshClient::_reset()
{
#if defined(DEBUG_SSHCLIENT)
    qDebug("DEBUG : SshClient : reset");
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
    QAbstractSocket::disconnectFromHost();
}

void SshClient::_disconnected()
{
    _keepalive.stop();
    if (_state != NoState)
    {
        qWarning("WARNING : SshClient : unexpected shutdown");
        _reset();
    }

    emit sshDisconnected();
}

void SshClient::_getLastError()
{
    char * msg;
    int len = 0;
    _errorcode = libssh2_session_last_error(_session, &msg, &len, 0);
    _errorMessage = QString::fromLocal8Bit(QByteArray::fromRawData(msg, len));
}


void SshClient::_delaydErrorEmit()
{
    emit sshError(_delayError);
    _errorcode = _delayError;
    emit _connectionTerminate();
}

