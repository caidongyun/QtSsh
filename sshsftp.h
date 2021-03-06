#ifndef SSHSFTP_H
#define SSHSFTP_H

#include "sshchannel.h"
#include <libssh2_sftp.h>
#include <QEventLoop>
#include <QTimer>
#include <QStringList>

class SshSFtp : public SshChannel
{
    Q_OBJECT

private:
    LIBSSH2_SFTP *_sftpSession;
    QString _mkdir;

    bool _waitData(int timeout);

public:
    SshSFtp(SshClient * client);
    ~SshSFtp();
    QString send(QString source, QString dest);
    bool get(QString source, QString dest, bool override = false);
    int mkdir(QString dest);
    QStringList dir(QString d);
    bool isDir(QString d);
    bool isFile(QString d);
    int mkpath(QString dest);
    bool unlink(QString d);

protected slots:
    void sshDataReceived();

signals:
    void sshData();
    void xfer();
};

#endif // SSHSFTP_H
