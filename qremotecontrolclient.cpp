#include "qremotecontrolclient.h"

#ifdef Q_OS_SYMBIAN
//#include "sym_iap_util.h"
#endif

QRemoteControlClient::QRemoteControlClient(QObject *parent)
    : QObject(parent)
{
//#ifndef LIGHTCONTROL
//    ui->pushButton_5->setVisible(false);
//#endif
//    ui->stackedWidget->setCurrentIndex(0);
//   ui->buttonStack->setCurrentIndex(0);
//    ui->songLabel->setText("");

    m_screenOrientation = ScreenOrientationAuto;
    m_version = QString(VERSION);
    m_screenDpi = QApplication::desktop()->physicalDpiX();

    loadSettings();

    tcpSocket = NULL;

    tcpServer =         new QTcpServer(this);
    udpSocket =         new QUdpSocket(this);
    broadcastTimer =    new QTimer(this);
    connectionRequestTimer = new QTimer(this);

    connect(tcpServer, SIGNAL(newConnection()), this, SLOT(newConnection()));
    connect(broadcastTimer, SIGNAL(timeout()), this, SLOT(sendBroadcast()));
    connect(connectionRequestTimer, SIGNAL(timeout()), this, SLOT(sendConnectionRequest()));

    // now begin the process of opening the network link
    netConfigManager = new QNetworkConfigurationManager;
    connect(netConfigManager, SIGNAL(updateCompleted()),
            this, SLOT(openNetworkSession()));
    netConfigManager->updateConfigurations();

    // initialize network timeout
    m_networkTimeout = 4500;
    initializeNetworkTimeoutTimer();
}

QRemoteControlClient::~QRemoteControlClient()
{
    saveSettings();

    if (tcpSocket)
        tcpSocket->disconnectFromHost();

    //delete ui;
}

void QRemoteControlClient::openNetworkSession()
{
    // use the default network configuration and make sure that
    // the link is open
    QNetworkConfiguration networkConfig;

    if ((netConfigManager->defaultConfiguration().bearerType() == QNetworkConfiguration::BearerEthernet)
            || (netConfigManager->defaultConfiguration().bearerType() == QNetworkConfiguration::BearerWLAN))
    {
        networkConfig = netConfigManager->defaultConfiguration();
    }
    else
    foreach (QNetworkConfiguration config, netConfigManager->allConfigurations(QNetworkConfiguration::Discovered))
    {
        if ((config.bearerType() == QNetworkConfiguration::BearerEthernet) ||
                (config.bearerType() == QNetworkConfiguration::BearerWLAN))
        {
            networkConfig = config;
#ifdef QT_DEBUG
            qDebug() << config.bearerTypeName() << config.bearerName() << config.name();
#endif
        }
    }

    if (networkConfig.isValid())
    {
        session = new QNetworkSession(networkConfig);

        if (session->isOpen()) {
            initialize();
        } else {
            connect(session, SIGNAL(opened()),
                    this, SLOT(initialize()));
            connect(session, SIGNAL(opened()),
                    this, SIGNAL(networkOpened()));
            connect(session, SIGNAL(closed()),
                    this, SIGNAL(networkClosed()));
            session->open();
        }
    }
}

bool QRemoteControlClient::sendWakeOnLan()
{
    WakeOnLanPacket wakeOnLanPacket;
    QByteArray macAddress = QByteArray();
    QString hostname = m_wolHostname;
    bool status = true;

    macAddress.append(m_wolMacAddress);

    if (!wakeOnLanPacket.setMacAddress(macAddress))
    {
#ifdef QT_DEBUG
        qDebug() << "mac address wrong";
#endif
        status = false;
    }

    if (hostname.isEmpty()) //empty means LAN
    {
        wakeOnLanPacket.setHostname(QString("255.255.255.255"));
    }
    else if (!wakeOnLanPacket.setHostname(hostname))
    {
#ifdef QT_DEBUG
        qDebug() << "hostname wrong";
#endif
        status = false;
    }

    if (!status)
    {
#ifdef QT_DEBUG
        qDebug() << "not sending magic packet";
#endif
        return status;
    }

    for (int i = 0; i < m_wolDatagramNumber; i++)   //send as often as needed
    {
        if (!wakeOnLanPacket.send())
            status = false;
    }

#ifdef QT_DEBUG
    if (!status)
    {
        qDebug() << "Probably no network connection";
    }
#endif

    return status;
}

void QRemoteControlClient::initialize()
{
    tcpServer->close();
    tcpServer->listen(QHostAddress::Any, m_port);

    udpSocket->close();
    connect(udpSocket, SIGNAL(readyRead()), this, SLOT(incomingUdpData()));
    udpSocket->bind(m_port, QUdpSocket::DontShareAddress | QUdpSocket::ReuseAddressHint);
}

void QRemoteControlClient::connectToHost()
{
    if (!m_password.isEmpty())
    {
        //here the host address should be added
        passwordHash = QCryptographicHash::hash(m_password.toUtf8(), QCryptographicHash::Sha1);
    }
    else
        passwordHash.clear();

    sendConnectionRequest();
    connectionRequestTimer->start(5000);

    emit connectingStarted();
}

void QRemoteControlClient::connectToServer(int id)
{
    QRCServer qrcServer = serverList[id];
    setHostAddress(qrcServer.hostAddress);
    abortBroadcasting();
    connectToHost();
}

void QRemoteControlClient::startBroadcasting()
{
    initialize();
    sendBroadcast();
    broadcastTimer->start(5000);

    emit broadcastingStarted();
}

void QRemoteControlClient::abortBroadcasting()
{
    broadcastTimer->stop();
}

void QRemoteControlClient::abortConnectionRequest()
{
    connectionRequestTimer->stop();
}

void QRemoteControlClient::saveSettings()
{
    QSettings settings("", "qremotecontrol", this);

    settings.setValue("password", m_password);
    settings.setValue("hostname", m_hostname);
    settings.setValue("port", m_port);
    settings.setValue("uiColor", m_uiColor);
    settings.setValue("uiRoundness", m_uiRoundness);
    settings.setValue("screenOrientation", static_cast<int>(m_screenOrientation));

    settings.beginGroup("wol");
        settings.setValue("macAddress", m_wolMacAddress);
        settings.setValue("hostname", m_wolHostname);
        settings.setValue("port", m_wolPort);
        settings.setValue("datagramNumber", m_wolDatagramNumber);
    settings.endGroup();

    settings.setValue("firstStart", false);
}

void QRemoteControlClient::loadSettings()
{
    QSettings settings("", "qremotecontrol", this);

    m_password  = settings.value("password", QString()).toString();
    m_hostname  = settings.value("hostname", QString()).toString();
    m_port      = settings.value("port", 5487).toInt();
    m_uiColor   = settings.value("uiColor", "black").toString();
    m_uiRoundness = settings.value("uiRoundness", 10).toDouble();
    m_screenOrientation = static_cast<ScreenOrientation>(settings.value("screenOrientation", ScreenOrientationAuto).toInt());

    settings.beginGroup("wol");
        m_wolMacAddress     = settings.value("macAddress",QString()).toString();
        m_wolHostname       = settings.value("hostname",QString()).toString();
        m_wolPort           = settings.value("port",80).toInt();
        m_wolDatagramNumber = settings.value("datagramNumber",5).toInt();
    settings.endGroup();

    if (settings.value("firstStart", true).toBool())
    {
        emit firstStart();
    }
}

void QRemoteControlClient::sendConnectionRequest()
{
    QByteArray datagram = "QRC:Hello";

    if (!passwordHash.isEmpty())        //for password protection
        datagram.append(passwordHash);

#ifdef QT_DEBUG
    qDebug() << "Connection request target: " << m_hostAddress;
#endif

    udpSocket->writeDatagram(datagram, m_hostAddress, m_port);
}

void QRemoteControlClient::sendBroadcast()
{
    QByteArray datagram = "QRC:Broadcast";

    udpSocket->writeDatagram(datagram, QHostAddress::Broadcast, m_port);
}

void QRemoteControlClient::newConnection()
{
    if (!tcpSocket) {
        tcpSocket = tcpServer->nextPendingConnection();

        connect(tcpSocket, SIGNAL(disconnected()), this, SLOT(deleteConnection()));
        connect(tcpSocket, SIGNAL(readyRead()), this, SLOT(incomingData()));

        emit connected();
        abortConnectionRequest();
        saveSettings();
        sendVersion();

        networkTimeoutTimer->start();
    }
}

void QRemoteControlClient::deleteConnection()
{
    tcpSocket->abort();
    tcpSocket->deleteLater();
    tcpSocket = NULL;

    networkTimeoutTimer->stop();

    emit disconnected();
}

void QRemoteControlClient::incomingData()
{
#ifdef QT_DEBUG
    qDebug() << "incoming data";
#endif
    QByteArray data = tcpSocket->readAll();
    int type = data.left(1).toInt();

    int payloadsize = data.mid(1,10).toInt();   //getting the size of the whole payload
                    data = data.mid(11);                        //removing obsolete data
                    while (data.size() != payloadsize) {        //wait for the missing data
                        tcpSocket->waitForReadyRead();
                        data.append(tcpSocket->readAll());
                    }

    switch (type) {
        case 1: incomingIcon(data);                         //process the data
                break;
        case 2: incomingAmarokData(data);
                break;
    }
}

void QRemoteControlClient::incomingUdpData()
{
    QHostAddress hostAddress;
#ifdef Q_OS_SYMBIAN
    QByteArray datagram(2^16,0);    //Workaround for a bug in Qt 4.8
#else
    QByteArray datagram;
    datagram.resize(udpSocket->pendingDatagramSize());
#endif

    udpSocket->readDatagram(datagram.data(), datagram.size(), &hostAddress);

    QString magicMessageConnected = "QRC:Connected";
    QString magicMessageNotConnected = "QRC:NotConnected";
    QString magicMessagePasswordIncorrect = "QRC:PasswordIncorrect";
    QString magicMessageServerConnecting = "QRC:ServerConnecting";

    if (datagram == magicMessageConnected)
    {
#ifdef QT_DEBUG
        qDebug() << "Server found, State: connected";
#endif
        addServer(hostAddress, true);
    }
    else if (datagram == magicMessageNotConnected)
    {
#ifdef QT_DEBUG
        qDebug() << "Server found, State: not connected" << hostAddress;
#endif
        addServer(hostAddress, false);
    }
    else if (datagram == magicMessagePasswordIncorrect)
    {
#ifdef QT_DEBUG
        qDebug() << "Password incorrect";
#endif
        emit passwordIncorrect();
    }
    else if (datagram == magicMessageServerConnecting)
    {
#ifdef QT_DEBUG
        qDebug() << "Server connecting";
#endif
        emit serverConnecting();
    }
}

void QRemoteControlClient::addServer(QHostAddress hostAddress, bool connected)
{
    bool found = false;
    for (int i = 0; i < serverList.size(); i++)
    {
        if (serverList.at(i).hostAddress == hostAddress) {
            found = true;
            serverList[i].connected = connected;
            break;
        }
    }
    if (!found)
    {
        QRCServer qrcServer;
        qrcServer.hostAddress = hostAddress;
        qrcServer.hostName = hostAddress.toString();
        qrcServer.connected = connected;
        serverList.append(qrcServer);

        QHostInfo::lookupHost(hostAddress.toString(),
                               this, SLOT(saveResolvedHostName(QHostInfo)));
    }

    emit serversCleared();
    for (int i = 0; i < serverList.size(); i++)
    {
        emit serverFound(serverList.at(i).hostAddress.toString(), serverList.at(i).hostName, serverList.at(i).connected);
    }
}

void QRemoteControlClient::initializeNetworkTimeoutTimer()
{
    networkTimeoutTimer = new QTimer(this);
    networkTimeoutTimer->setInterval(m_networkTimeout);
    connect(networkTimeoutTimer, SIGNAL(timeout()),
            this, SLOT(sendKeepAlive()));
}

void QRemoteControlClient::saveResolvedHostName(QHostInfo hostInfo)
{
    for (int i = 0; i < serverList.size(); i++)
    {
        if (serverList.at(i).hostName == hostInfo.addresses().first().toString())
        {
            serverList[i].hostName = hostInfo.hostName();
        }
    }

    emit serversCleared();
    for (int i = 0; i < serverList.size(); i++)
    {
        emit serverFound(serverList.at(i).hostAddress.toString(), serverList.at(i).hostName, serverList.at(i).connected);
    }
}

void QRemoteControlClient::clearServerList()
{
    serverList.clear();
    emit serversCleared();
}

void QRemoteControlClient::incomingIcon(QByteArray data)
{
    QDataStream in(data);
    while (!in.atEnd()) {
        quint8 id;
        QPixmap icon;
        QString name;
        QString filePath = QDir::tempPath() + "/";
        in >> id >> name >> icon;
        if (!icon.isNull())
        {
            filePath += QString("qrc%1.png").arg(id);

            if (QFile::exists(filePath))
                QFile::remove(filePath);

            icon.save(filePath);
        }

        actionReceived(id, name, filePath);
    }
}

void QRemoteControlClient::incomingAmarokData(QByteArray data)
{
#ifdef QT_DEBUG
    qDebug() << "incoming Amarok data";
#endif
    QDataStream in(data);
    QString title,
            artist;
    in >> title >> artist;

    //ui->songLabel->setText(tr("%1<br>%2").arg(title,
    //                                          artist)); //GUI
}

void QRemoteControlClient::sendKey(quint32 key, quint32 modifiers,  bool keyPressed)
{   
    quint8 mode1 = 1;

    QByteArray data;
    QDataStream streamOut(&data, QIODevice::WriteOnly);
    streamOut << mode1;
    streamOut << key;
    streamOut << modifiers;
    streamOut << keyPressed;

    udpSocket->writeDatagram(data, tcpSocket->peerAddress(), m_port);
}

void QRemoteControlClient::sendButton(quint8 id, bool keyPressed)
{
    quint8 mode1 = 5;

    QByteArray data;
    QDataStream streamOut(&data, QIODevice::WriteOnly);
    streamOut << mode1;
    streamOut << id;
    streamOut << keyPressed;

    udpSocket->writeDatagram(data, tcpSocket->peerAddress(), m_port);
}

void QRemoteControlClient::sendKeyPress(quint8 id)
{
    sendButton(id, true);
}

void QRemoteControlClient::sendKeyRelease(quint8 id)
{
   sendButton(id, false);
}

 void QRemoteControlClient::sendMouseMove(int deltaX, int deltaY)
 {
     QPoint delta(deltaX, deltaY);

     if (delta == QPoint(0,0))
         return;

    quint8 mode1 = 2;
    quint8 mode2 = 0;

    QByteArray data;
    QDataStream streamOut(&data, QIODevice::WriteOnly);
    streamOut << mode1;
    streamOut << mode2;
    streamOut << delta;

    udpSocket->writeDatagram(data, tcpSocket->peerAddress(), m_port);
 }

 void QRemoteControlClient::sendHorizontalScroll(int delta)
 {
     sendMouseScroll(1,delta);
 }

 void QRemoteControlClient::sendVerticalScroll(int delta)
 {
     sendMouseScroll(2,delta);
 }

 void QRemoteControlClient::sendMouseScroll(quint8 direction, qint8 delta)
 {
     if (delta == 0)
         return;

     quint8 mode1 = 2;
     quint8 mode2 = 2;

     QByteArray data;
     QDataStream streamOut(&data, QIODevice::WriteOnly);
     streamOut << mode1;
     streamOut << mode2;
     streamOut << direction;
     streamOut << delta;

     udpSocket->writeDatagram(data, tcpSocket->peerAddress(), m_port);
 }

 void QRemoteControlClient::sendMouseButton(quint8 button, bool buttonPressed)
 {
     quint8 mode1 = 2;
     quint8 mode2 = 1;

     QByteArray data;
     QDataStream streamOut(&data, QIODevice::WriteOnly);
     streamOut << mode1;
     streamOut << mode2;
     streamOut << button;
     streamOut << buttonPressed;

     udpSocket->writeDatagram(data, tcpSocket->peerAddress(), m_port);
 }

 void QRemoteControlClient::sendMousePress(quint8 button)
 {
     sendMouseButton(button, true);
 }

 void QRemoteControlClient::sendMouseRelease(quint8 button)
 {
     sendMouseButton(button, false);
 }

 void QRemoteControlClient::sendControlClicked(bool down)
 {
     sendKey(Qt::Key_unknown, Qt::ControlModifier, down);
 }

 void QRemoteControlClient::sendAltClicked(bool down)
 {
     sendKey(Qt::Key_unknown, Qt::AltModifier, down);
 }

 void QRemoteControlClient::sendShiftClicked(bool down)
 {
     sendKey(Qt::Key_unknown, Qt::ShiftModifier, down);
 }

void QRemoteControlClient::sendAction(int id, bool pressed)
{
    quint8 mode1 = 3;

    QByteArray data;
    QDataStream streamOut(&data, QIODevice::WriteOnly);
    streamOut << mode1;
    streamOut << (quint8)id;
    streamOut << pressed;

    udpSocket->writeDatagram(data, tcpSocket->peerAddress(), m_port);
}

void QRemoteControlClient::sendLight(int code)
{
    quint8 mode1 = 4;

    QByteArray data;
    QDataStream streamOut(&data, QIODevice::WriteOnly);
    streamOut << mode1;
    streamOut << (quint16)code;

    udpSocket->writeDatagram(data, tcpSocket->peerAddress(), m_port);
}

void QRemoteControlClient::sendKeepAlive()
{
    quint8 mode1 = 6;

    QByteArray data;
    QDataStream streamOut(&data, QIODevice::WriteOnly);
    streamOut << mode1;

    udpSocket->writeDatagram(data, tcpSocket->peerAddress(), m_port);
}

void QRemoteControlClient::sendVersion()
{
    quint8 mode1 = 7;

    QByteArray data;
    QDataStream streamOut(&data, QIODevice::WriteOnly);
    streamOut << mode1;
    streamOut << QString(VERSION);

    udpSocket->writeDatagram(data, tcpSocket->peerAddress(), m_port);
}

void QRemoteControlClient::disconnect()
{
    deleteConnection();
}

/*void QRemoteControlClient::on_closeButton_clicked()
{
    close();
}

void QRemoteControlClient::on_aboutButton_clicked()
{
    QMessageBox::about(this, tr("About QRemoteControl"), tr("<p align=center>"
                                                 "<nobr><b>QRemoteControl %1</b></nobr> <br><br>"
                                                 "<nobr>Copyright 2009-2012 by</nobr><br>"
                                                 "<nobr>Alexander R&ouml;ssler</nobr>"
                                                 "</p>").arg(VERSION));
}

void QRemoteControlClient::on_pushButton_1_clicked()
{
    ui->buttonStack->setCurrentIndex(0);
    ui->pushButton_2->setChecked(false);
    ui->pushButton_3->setChecked(false);
    ui->pushButton_4->setChecked(false);
    ui->pushButton_5->setChecked(false);
    this->setOrientation(QRemoteControlClient::ScreenOrientationLockPortrait);
}

void QRemoteControlClient::on_pushButton_2_clicked()
{
    ui->buttonStack->setCurrentIndex(1);
    ui->pushButton_1->setChecked(false);
    ui->pushButton_3->setChecked(false);
    ui->pushButton_4->setChecked(false);
    ui->pushButton_5->setChecked(false);
    this->setOrientation(QRemoteControlClient::ScreenOrientationLockPortrait);
}

void QRemoteControlClient::on_pushButton_3_clicked()
{
    ui->buttonStack->setCurrentIndex(2);
    ui->pushButton_1->setChecked(false);
    ui->pushButton_2->setChecked(false);
    ui->pushButton_4->setChecked(false);
    ui->pushButton_5->setChecked(false);
    this->setOrientation(QRemoteControlClient::ScreenOrientationLockPortrait);
}

void QRemoteControlClient::on_pushButton_4_clicked()
{
    ui->buttonStack->setCurrentIndex(3);
    ui->pushButton_1->setChecked(false);
    ui->pushButton_2->setChecked(false);
    ui->pushButton_3->setChecked(false);
    ui->pushButton_5->setChecked(false);
    this->setOrientation(QRemoteControlClient::ScreenOrientationAuto);

    ui->keyboardEdit->setFocus();
}

void QRemoteControlClient::on_pushButton_5_clicked()
{
    ui->buttonStack->setCurrentIndex(4);
    ui->pushButton_1->setChecked(false);
    ui->pushButton_2->setChecked(false);
    ui->pushButton_3->setChecked(false);
    ui->pushButton_4->setChecked(false);
    this->setOrientation(QRemoteControlClient::ScreenOrientationLockPortrait);
}

void QRemoteControlClient::on_leftMouseButton_pressed()
{
    sendMousePress(1);
}

void QRemoteControlClient::on_leftMouseButton_released()
{
    sendMouseRelease(1);
}

void QRemoteControlClient::on_middleMouseButton_pressed()
{
     sendMousePress(2);
}

void QRemoteControlClient::on_middleMouseButton_released()
{
    sendMouseRelease(2);
}

void QRemoteControlClient::on_rightMouseButton_pressed()
{
    sendMousePress(3);
}

void QRemoteControlClient::on_rightMouseButton_released()
{
    sendMouseRelease(3);
}

void QRemoteControlClient::on_redButton_clicked()
{
    sendLight(15);
}

void QRemoteControlClient::on_greenButton_clicked()
{
    sendLight(240);
}

void QRemoteControlClient::on_blueButton_clicked()
{
    sendLight(3840);
}

void QRemoteControlClient::on_yellowButton_clicked()
{
    sendLight(255);
}

void QRemoteControlClient::on_cyanButton_clicked()
{
    sendLight(4080);
}

void QRemoteControlClient::on_magentaButton_clicked()
{
    sendLight(3855);
}

void QRemoteControlClient::on_blackButton_clicked()
{
    sendLight(0);
}

void QRemoteControlClient::on_whiteButton_clicked()
{
    sendLight(4095);
}

void QRemoteControlClient::on_codeSpin_valueChanged(int value)
{
    sendLight(value);
}

void QRemoteControlClient::on_connectButton_clicked()
{
    setHostname(ui->hostnameEdit->text());
    setPassword(ui->passwordEdit->text());
    setPort(ui->portSpin->value());

    connectToHost();
}

void QRemoteControlClient::on_abortButton_clicked()
{
    abortBroadcasting();
    ui->stackedWidget->setCurrentIndex(0);  //GUI
}

void QRemoteControlClient::on_continueButton_clicked()
{
    ui->stackedWidget->setCurrentIndex(0);
}

void QRemoteControlClient::on_helpButton_clicked()
{
    ui->stackedWidget->setCurrentIndex(3);
}*/
