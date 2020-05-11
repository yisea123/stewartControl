#include <iostream>
#include "controller.h"
#include "GAS_N.h"
QMutex m_mutex;
QMutex imu_mutex;
QWaitCondition m_cond;
QByteArray globalByteArray;
QByteArray globalGyroArray;
Controller::Controller(char* com_card, char* com_modbus, char* com_imu, QObject* parent):QObject(parent),timer(new QTimer),RS485(new modbusController),tcpServer(new QTcpServer(this)),uart(new SerialPort)
{
    //stewartPara *para=new stewartPara(170, 80, 290, 47);
    stewartPara *para=new stewartPara(170, 80, 280, 47);
    kinematicModule=new inverseKinematic(para);
    if(GA_Open(0,com_card))
        qDebug()<<"open card failed";
    else
        qDebug()<<"open card successful";
    if(GA_Reset())
        qDebug()<<"reset card failed";
    else
        qDebug()<<"reset card successful";
    connect(this,&Controller::startUartSignal,uart,&SerialPort::start_port);
    connect(uart,&SerialPort::receive_data,this,&Controller::updatePosition);
    connect(this,&Controller::sendReadRequestSignal,RS485,&modbusController::sendReadRequestSlot);
    connect(this,&Controller::sendWriteRequestSignal,RS485,&modbusController::sendWriteRequestSlot);
    connect(this,&Controller::initModbusSignal,RS485,&modbusController::initModbusSlot);
    connect(this,&Controller::sendSocketSignal,uart,&SerialPort::sendSocketSlot);
    emit startUartSignal(QString(com_imu),115200,2);
    emit initModbusSignal(com_modbus);
    //for(int i=1;i<=6;i++)
    //    setDriverEnable(i,true);
    for(int i=0;i<6;i++)
        pid_regulator[i]=new PIDController(0.015,0.001,0.02,50,10,10,40,-40);
    tcpServer->listen(QHostAddress::Any,8088);
    connect(tcpServer,&QTcpServer::newConnection,this,[this]()
    {
        QTcpSocket* tcpClient=tcpServer->nextPendingConnection();
        QString ip = tcpClient->peerAddress().toString();
        quint16 port = tcpClient->peerPort();
        qDebug()<<"one connection established from ip: "<<ip<<" port: "<<port;
        this->tcpClients << tcpClient;
        tcpClient->setParent(this);
        connect(tcpClient,&QTcpSocket::readyRead,this,&Controller::tcpReadDataSlot);
        connect(tcpClient,&QTcpSocket::disconnected,this,[this]()
        {
            QTcpSocket *pClient = qobject_cast<QTcpSocket *>(sender());
            QString ip = pClient->peerAddress().toString();
            quint16 port = pClient->peerPort();
            qDebug()<<"one connection closed from ip: "<<ip<<" port: "<<port;
            if (pClient)
            {
                tcpClients.removeAll(pClient);
                if(imuClient==pClient)
                    imuClient=nullptr;
                pClient->deleteLater();
            }
        });
    });
    legIndex2Motion[0]=1;
    legIndex2Motion[1]=6;
    legIndex2Motion[2]=5;
    legIndex2Motion[3]=4;
    legIndex2Motion[4]=3;
    legIndex2Motion[5]=2;
    for(auto it=legIndex2Motion.begin();it!=legIndex2Motion.end();it++)
        motion2LegIndex[it->second]=it->first;
    refPos.resize(6);
#if HARDLIMITS
    int res=0;
    for(short i=1;i<=6;i++)
    {
        res+=GA_LmtsOn(i,-1);
        res+=GA_SetHardLimP(i,1,0,i-1);
        res+=GA_SetHardLimN(i,1,0,i+5);
        if(res)
            qDebug()<<"set hard limits failed!!";
    }
#endif
}

void Controller::simpleOperationMode()
{
    currentStatus=Status::Simple;
    short axis=6;
    if(GA_PrfTrap(axis))
    {
        qDebug()<<"set trap model failed";
        return;
    }
    TTrapPrm trapPrm;
    if(GA_GetTrapPrm(axis,&trapPrm))
    {
        qDebug()<<"set trap model failed";
        return;
    }
    trapPrm.acc=0.5;
    trapPrm.dec=0.5;
    trapPrm.velStart=0;
    trapPrm.smoothTime=0;
    if(GA_SetTrapPrm(axis,&trapPrm))
    {
        qDebug()<<"set trap params failed";
        return;
    }
    if(GA_SetVel(axis,1))
    {
        qDebug()<<"set speed failed";
        return;
    }

    connect(timer,&QTimer::timeout,this,&Controller::timerSlot);
    timer->setInterval(50);
    timer->start();
}
void Controller::timerSlot()
{
    short axis=6;
    long status=0;
    if(GA_GetSts(axis,&status))
    {
        qDebug()<<"read status failed";
        GA_Stop(0x0001<<(axis-1),0x0001<<(axis-1));
        return;
    }
    if(status & AXIS_STATUS_RUNNING)
    {
        qDebug()<<"running";
        return;
    }
    double pos;static int cnt=0;
    if(GA_GetAxisPrfPos(axis,&pos))
    {
        qDebug()<<"read pos failed";
        GA_Stop(0x0001<<(axis-1),0x0001<<(axis-1));
        return;
    }
    if(cnt%2)
    {
        if(GA_SetPos(axis,pos+10))
        {
            qDebug()<<"set pos failed";
            GA_Stop(0x0001<<(axis-1),0x0001<<(axis-1));
            return;
        }
        if(GA_Update(0x0001<<(axis-1)))
        {
            qDebug()<<"update failed";
            GA_Stop(0x0001<<(axis-1),0x0001<<(axis-1));
            return;
        }
    }
    else
    {
        if(GA_SetPos(axis,pos-10))
        {
            qDebug()<<"set pos failed";
            GA_Stop(0x0001<<(axis-1),0x0001<<(axis-1));
            return;
        }
        if(GA_Update(0x0001<<(axis-1)))
        {
            qDebug()<<"update failed";
            GA_Stop(0x0001<<(axis-1),0x0001<<(axis-1));
            return;
        }
    }
    cnt++;
    if(cnt>1)
        timer->stop();
}

void Controller::reset(int addr)
{
    QVariant data;
    QVector<quint16> dd;
    dd.append(1);dd.append(0);
    data.setValue(dd);
    m_mutex.lock();
    emit sendWriteRequestSignal(addr,2030,2,data);
    m_cond.wait(&m_mutex);
    m_mutex.unlock();
}
void Controller::resetAll(int start, int end)
{
    for(int i=start;i<=end;i++)
        reset(i);
    QVector<int> t(6,0);
    int cntZros=0;
    while(true)
    {
        for(int i=start;i<=end;i++)
            GetZphasePos(i);
        std::cout<<"Current Pos: ";
        for(int i=start-1;i<end;i++)
        {
            std::cout<<"|addr"<<i+1<<" "<<RS485->zPhasePos[i]->GetCurrentPos()<<"| ";
            if(abs(RS485->zPhasePos[i]->GetCurrentPos())<=50 && t[i]==0)
            {
                cntZros++;
                t[i]=1;
            }
        }
        std::cout<<"\n";
        if(cntZros==(end-start+1))
            break;
        //QThread::msleep(50);
    }
    qDebug()<<"Mortor Reset Finished";
}
void Controller::resetAll()
{
    for(int i=1;i<=6;i++)
    {
        reset(i);
        GA_ZeroPos(i,1);
        GA_ClrSts(i,1);
    }
    QVector<int> t(6,0);
    int cntZros=0;
    while(true)
    {
        for(int i=1;i<=6;i++)
            GetZphasePos(i);
        std::cout<<"Current Pos: ";
        for(int i=0;i<6;i++)
        {
            std::cout<<"|addr"<<i+1<<" "<<RS485->zPhasePos[i]->GetCurrentPos()<<"| ";
            if((abs(RS485->zPhasePos[i]->GetCurrentPos())<=50 || abs(10000-RS485->zPhasePos[i]->GetCurrentPos())<=50) && t[i]==0)
            {
                cntZros++;
                t[i]=1;
            }
        }
        std::cout<<"\n";
        if(cntZros==6)
            break;
        //QThread::msleep(50);
    }
    qDebug()<<"Mortor Reset Finished";
}

void Controller::GetZphasePos(int addr)
{
    m_mutex.lock();
    emit sendReadRequestSignal(addr,4032,2);
    m_cond.wait(&m_mutex);
    m_mutex.unlock();
}
void Controller::setDriverEnable(int addr, bool value)
{
    QVariant data;
    QVector<quint16> dd;
    dd.append(value==true?1:0);dd.append(0);
    data.setValue(dd);
    m_mutex.lock();
    emit sendWriteRequestSignal(addr,1008,2,data);
    m_cond.wait(&m_mutex);
    m_mutex.unlock();
}
void Controller::GetCurrentPos(int addr)
{
    m_mutex.lock();
    emit sendReadRequestSignal(addr,4008,4);
    m_cond.wait(&m_mutex);
    m_mutex.unlock();
}
void Controller::MoveLegs(QVector<double>& pos,MotionMode mode)
{
    TIMEBEGIN()
    if(mode==MotionMode::JOG)
        GA_GetPrfPos(1,currentPos,6);
    TIMEEND("Get Pos:")
    TIMEBEGIN()
    for(int i=1;i<=6;i++)
    {
        if(mode==MotionMode::TRAP)
            MoveLeg(legIndex2Motion[i-1], kinematicModule->Len2Pulse(pos[i-1]));
        else if(mode==MotionMode::JOG)
            MoveLegInJog(legIndex2Motion[i-1],-kinematicModule->Len2Pulse(pos[i-1]),currentPos[legIndex2Motion[i-1]-1]);
        qDebug() << "Leg: "<<i<<" length: "<< pos[i-1];
    }
    TIMEEND("Set Speed:")
    TIMEBEGIN()
    updateAxis(1,6);
    TIMEEND("Update Axis:")
}
void Controller::MoveLegInJog(int addr, qint64 pos,double _currentPos)
{
    pid_regulator[addr-1]->setRef(pos);
    pid_regulator[addr-1]->setFeedBack(_currentPos);
    pid_regulator[addr-1]->calculate();
    qDebug()<<"fdb:"<<_currentPos<<" ref:"<<pos<<" output:"<<pid_regulator[addr-1]->GetOutput();
    GA_SetVel(addr,pid_regulator[addr-1]->GetOutput()+refSpeed[motion2LegIndex[addr]]*0.25);
}
void Controller::MoveLeg(int addr, qint64 pos, bool flag)
{
#if FEEDBACK_FROM_MORTOR
    GetCurrentPos(addr);
    qint64 relatedPos=pos-RS485->GeneralData[addr-1];
    qDebug()<<"pos: "<<pos<<" current pos: "<<RS485->GeneralData[addr-1]<<"related pos: "<<relatedPos;
    double currentPos;
    GA_GetAxisPrfPos(static_cast<short>(addr),&currentPos);
    qDebug()<<"read from card: "<<currentPos;
    GA_SetPos(addr, currentPos-relatedPos);
#else
    GA_SetPos(addr, -pos);
#endif
    if(flag)
    {
        if(GA_Update(0x0001<<(addr-1)))
        {
            qDebug()<<"update failed when move leg: "<<addr;
            GA_Stop(0x0001<<(addr-1),0x0001<<(addr-1));
        }
    }
}
void Controller::updateAxis(int start, int end)
{
    uint8_t val=0;
    for(int i=start;i<=end;i++)
        val=val|(0x0001<<(i-1));
    if(GA_Update(val))
        qDebug()<<"update failed when move legs";
}
void Controller::initMode(double acc,double dec,double speed,MotionMode mode)
{
    for(short axis=1;axis<=6;axis++)
    {
        if(mode==MotionMode::TRAP)
        {
            if(GA_PrfTrap(axis))
            {
                qDebug()<<"set trap model failed";
                return;
            }
            TTrapPrm trapPrm;
            if(GA_GetTrapPrm(axis,&trapPrm))
            {
                qDebug()<<"read trap model failed";
                return;
            }
            trapPrm.acc=acc;
            trapPrm.dec=dec;
            trapPrm.velStart=0;
            trapPrm.smoothTime=0;
            if(GA_SetTrapPrm(axis,&trapPrm))
            {
                qDebug()<<"set trap params failed";
                return;
            }
            if(GA_SetVel(axis,speed))
            {
                qDebug()<<"set speed failed";
                return;
            }
        }
        else if(mode==MotionMode::JOG)
        {
            if(GA_PrfJog(axis))
            {
                qDebug()<<"set JOG model failed";
                return;
            }
            JogPrm jogPrm;
            if(GA_GetJogPrm(axis,&jogPrm))
            {
                qDebug()<<"read JOG model failed";
                return;
            }
            jogPrm.dAcc=acc;
            jogPrm.dDec=dec;
            jogPrm.dSmooth=0;
            if(GA_SetJogPrm(axis,&jogPrm))
            {
                qDebug()<<"set JOG params failed";
                return;
            }
            if(GA_SetVel(axis,speed))
            {
                qDebug()<<"set speed failed";
                return;
            }
        }
    }
}
void Controller::GuiControlMode()
{
    currentStatus=Status::GUIControl;
    initMode(2,2,5,MotionMode::TRAP);
    QVector<double> len = kinematicModule->GetLength(0,0,normalZ,0,0,0);
    MoveLegs(len);
}
void Controller::IMUControlMode()
{
    currentStatus=Status::IMUControl;
    initMode(2,2,5,MotionMode::TRAP);
    QVector<double> lens = kinematicModule->GetLength(0,0,normalZ,0,0,0);
    qDebug()<<"start move legs home";
    MoveLegs(lens);
    short axis=1;
    long status=0;
    while(axis<=6)
    {
        if(GA_GetSts(axis,&status))
        {
            qDebug()<<"read status failed";
            GA_Stop(0x0001<<(axis-1),0x0001<<(axis-1));
        }
        if(status & AXIS_STATUS_RUNNING)
            QThread::msleep(10);
        else
            axis++;
    }
    qDebug()<<"move home finished";
    initMode(3,3,10,MotionMode::JOG);
    connect(timer,&QTimer::timeout,this,[this]()
    {
        TIMEBEGIN()
        analyseData();
        qDebug()<<"angleX: "<<angleX<<" "<<"angleY: "<<angleY<<"angleZ: "<<angleZ;
#if HARDLIMITS
        GA_ClrSts(1,6);
#endif
        //refSpeed = kinematicModule->GetSpeed(gyroX,gyroY,gyroZ);
        refSpeed = kinematicModule->GetSpeed(gyroX,gyroY,0);
        refPos = kinematicModule->GetLength(0,0,normalZ,-angleX,-angleY,0);
        MoveLegs(refPos,MotionMode::JOG);
        TIMEBEGIN()
        sendData();
        TIMEEND("Send Data:")
        TIMEEND("Total:")
    });
    timer->setInterval(1);
    timer->start();
}
void Controller::tcpReadDataSlot()
{
    QTcpSocket *pClient = qobject_cast<QTcpSocket *>(sender());
    QByteArray array = pClient->readAll();
    static int state=0;
    static QVector<double> posData;
    int dataLen=array.size();
    for(int i=0;i<dataLen;i++)
    {
        if(state==0 && static_cast<uint8_t>(array.at(i))==0xaa)
        {
            posData.clear();
            state++;
        }
        else if(state==1)
        {
            auto d=static_cast<uint8_t>(array.at(i));
            if(d!=0xaa && d!=0xbb)
                state=0;
            else if(d==0xaa)
                state++;
            else if(d==0xbb)
            {
                state=0;
                imuClient=pClient;
                qDebug()<<"data display conn established";
                //emit sendSocketSignal(pClient);
            }
        }
        else if(state>=2)
        {
            posData.append(static_cast<char>(array.at(i)));
            state++;
            if(state>=8)
            {
                state=0;
                if(currentStatus==GUIControl)
                {
                    currentX=posData[0];currentY=posData[1];currentZ=posData[2]+normalZ;currentRx=posData[3];currentRy=posData[4];currentRz=posData[5];
                    QVector<double> len = kinematicModule->GetLength(currentX,currentY,currentZ,currentRx,currentRy,currentRz);
                    MoveLegs(len);
                    qDebug()<<"**********************\nrecieve target:"<<currentX<<currentY<<currentZ-normalZ<<currentRx<<currentRy<<currentRz;
                }
            }
        }
    }
    //qDebug()<<array;
}
void Controller::simpleTrajectory(int mode)
{
    qDebug()<<"enter simple trajectory mode\n";
    double diff_height=20;
    double x_amp=60;
    bool dir=true;
    auto lens=kinematicModule->GetLength(0,0,normalZ+diff_height,0,0,0);
    MoveLegs(lens);
    QThread::sleep(1);
    int cnt=0,total=30;
    while(true)
    {
        if(mode==0)
        {
            if(dir)
            {
                auto lens=kinematicModule->GetLength(x_amp,0,normalZ+diff_height,0,0,0);
                MoveLegs(lens);
                dir=false;
            }
            else
            {
                auto lens=kinematicModule->GetLength(-x_amp,0,normalZ+diff_height,0,0,0);
                MoveLegs(lens);
                dir=true;
            }
            QThread::sleep(2);
        }
        else if(mode==1)
        {
            auto lens=kinematicModule->GetLength(x_amp/total*cnt,0,normalZ+diff_height,0,0,0);
            MoveLegs(lens);
            if(dir)
            {
                cnt++;
                if(cnt>total)
                    dir=false;
            }
            else
            {
                cnt--;
                if(cnt<-total)
                    dir=true;
            }
            QThread::msleep(10);
        }
    }
}
void Controller::updatePosition(QByteArray data)
{
    auto type=static_cast<recieveType>(data.at(0));
    if(type==recieveType::angle)
    {
        angleX=static_cast<int16_t>(((static_cast<uint8_t>(data.at(2))<<8)|static_cast<uint8_t>(data.at(1))))/32768.0*180.0;
        angleY=static_cast<int16_t>(((static_cast<uint8_t>(data.at(4))<<8)|static_cast<uint8_t>(data.at(3))))/32768.0*180.0;
        angleZ=static_cast<int16_t>(((static_cast<uint8_t>(data.at(6))<<8)|static_cast<uint8_t>(data.at(5))))/32768.0*180.0;
        if(imuClient!=nullptr)
        {
            for(int i=0;i<6;i++)
                data.append(static_cast<char>(pid_regulator[i]->GetOutput()));
            for(int i=0;i<6;i++)
            {
                data.append(static_cast<char>(static_cast<int16_t>(currentPos[i])>>8));
                data.append(static_cast<char>(static_cast<int16_t>(currentPos[i])&0x00ff));
            }
            for(int i=0;i<6;i++)
            {
                auto puls=kinematicModule->Len2Pulse(refPos[motion2LegIndex[i+1]]);
                data.append(static_cast<char>(static_cast<int16_t>(-puls)>>8));
                data.append(static_cast<char>(static_cast<int16_t>(-puls)&0x00ff));
            }
            imuClient->write(data);
        }
    }
    else if(type==recieveType::gyro)
    {
        gyroX=static_cast<int16_t>(((static_cast<uint8_t>(data.at(2))<<8)|static_cast<uint8_t>(data.at(1))))/32768.0*2000.0;
        gyroY=static_cast<int16_t>(((static_cast<uint8_t>(data.at(4))<<8)|static_cast<uint8_t>(data.at(3))))/32768.0*2000.0;
        gyroZ=static_cast<int16_t>(((static_cast<uint8_t>(data.at(6))<<8)|static_cast<uint8_t>(data.at(5))))/32768.0*2000.0;
    }
    //qDebug()<<angleX<<" "<<angleY<<" "<<angleZ;
}
void Controller::analyseData()
{
    imu_mutex.lock();
    auto data=globalByteArray;
    auto data2=globalGyroArray;
    imu_mutex.unlock();
    if(data.size())
    {
        angleX=static_cast<int16_t>(((static_cast<uint8_t>(data.at(2))<<8)|static_cast<uint8_t>(data.at(1))))/32768.0*180.0;
        angleY=static_cast<int16_t>(((static_cast<uint8_t>(data.at(4))<<8)|static_cast<uint8_t>(data.at(3))))/32768.0*180.0;
        angleZ=static_cast<int16_t>(((static_cast<uint8_t>(data.at(6))<<8)|static_cast<uint8_t>(data.at(5))))/32768.0*180.0;
    }
    if(data2.size())
    {
        gyroX=static_cast<int16_t>(((static_cast<uint8_t>(data2.at(2))<<8)|static_cast<uint8_t>(data2.at(1))))/32768.0*2000.0;
        gyroY=static_cast<int16_t>(((static_cast<uint8_t>(data2.at(4))<<8)|static_cast<uint8_t>(data2.at(3))))/32768.0*2000.0;
        gyroZ=static_cast<int16_t>(((static_cast<uint8_t>(data2.at(6))<<8)|static_cast<uint8_t>(data2.at(5))))/32768.0*2000.0;
    }
}
void Controller::sendData()
{
    imu_mutex.lock();
    auto data=globalByteArray;
    auto data2=globalGyroArray;
    imu_mutex.unlock();
    if(data.size() && data2.size())
    {
        if(imuClient!=nullptr)
        {
            for(int i=0;i<6;i++)
                data.append(static_cast<char>(pid_regulator[i]->GetOutput()));
            for(int i=0;i<6;i++)
            {
                data.append(static_cast<char>(static_cast<int16_t>(currentPos[i])>>8));
                data.append(static_cast<char>(static_cast<int16_t>(currentPos[i])&0x00ff));
            }
            for(int i=0;i<6;i++)
            {
                auto puls=kinematicModule->Len2Pulse(refPos[motion2LegIndex[i+1]]);
                data.append(static_cast<char>(static_cast<int16_t>(-puls)>>8));
                data.append(static_cast<char>(static_cast<int16_t>(-puls)&0x00ff));
            }
            for(int i=0;i<6;i++)
            {
                auto speed=kinematicModule->Speed2Pulse(refSpeed[motion2LegIndex[i+1]]);
                for(int j=0;j<4;j++)
                    data.append(static_cast<char>(static_cast<int32_t>(speed)>>(8*(3-j))));
            }
            data.append(data2);
            imuClient->write(data);
        }
    }
}
