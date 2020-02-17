#include <QDebug>
#include <QModbusRtuSerialMaster>
#include <iostream>
#include <QtMath>
#include "controller.h"
#include "GAS_N.h"
QMutex m_mutex;
QWaitCondition m_cond;
Controller::Controller(char* com_card,char* com_modbus,QObject* parent):QObject(parent),timer(new QTimer),RS485(new modbusController),tcpServer(new QTcpServer(this))
{
    stewartPara *para=new stewartPara(10,10, -10*M_PI/180, 80,5);
    kinematicModule=new inverseKinematic(para);
    if(GA_Open(1,com_card))
        qDebug()<<"open card failed";
    else
        qDebug()<<"open card successful";
    connect(this,&Controller::sendReadRequestSignal,RS485,&modbusController::sendReadRequestSlot);
    connect(this,&Controller::sendWriteRequestSignal,RS485,&modbusController::sendWriteRequestSlot);
    connect(this,&Controller::initModbusSignal,RS485,&modbusController::initModbusSlot);
    //emit initModbusSignal(com_modbus);
    //for(int i=1;i<=6;i++)
    //    setDriverEnable(i,true);
    tcpServer->listen(QHostAddress::Any,8088);
    connect(tcpServer,&QTcpServer::newConnection,this,[this]()
    {
        QTcpSocket* tcpClient=tcpServer->nextPendingConnection();
        QString ip = tcpClient->peerAddress().toString();
        quint16 port = tcpClient->peerPort();
        qDebug()<<"one connection established from ip: "<<ip<<" port: "<<port;
        this->tcpClients << tcpClient;
        tcpClient->setParent(this);
        connect(tcpClient,&QTcpSocket::readyRead,this,[this]()
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
                    if(static_cast<uint8_t>(array.at(i))==0xaa)
                        state++;
                    else
                        state=0;
                }
                else if(state>=2)
                {
                    posData.append(static_cast<uint8_t>(array.at(i)));
                    state++;
                    if(state>=8)
                    {
                        state=0;
                        currentX=posData[0];currentY=posData[1];currentZ=posData[2];currentRx=posData[3];currentRy=posData[4];currentRz=posData[5];
                    }
                }
            }
            //qDebug()<<array;
        });
        connect(tcpClient,&QTcpSocket::disconnected,this,[this]()
        {
            QTcpSocket *pClient = qobject_cast<QTcpSocket *>(sender());
            QString ip = pClient->peerAddress().toString();
            quint16 port = pClient->peerPort();
            qDebug()<<"one connection closed from ip: "<<ip<<" port: "<<port;
            if (pClient)
            {
                tcpClients.removeAll(pClient);
                pClient->deleteLater();
            }
        });
    });
}

void Controller::simpleOperation()
{
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
void Controller::resetAll()
{
    for(int i=1;i<=6;i++)
        reset(i);
    while(true)
    {
        for(int i=1;i<=6;i++)
            GetZphasePos(i);
        std::cout<<"Current Pos: ";
        int cntZros=0;
        for(int i=0;i<6;i++)
        {
            std::cout<<"|addr"<<i+1<<" "<<RS485->zPhasePos[i]->GetCurrentPos()<<"| ";
            if(RS485->zPhasePos[i]->GetCurrentPos()==0)
                cntZros++;
        }
        std::cout<<"\n";
        if(cntZros==6)
            break;
        QThread::msleep(50);
    }
    qDebug()<<"Mortor Reset Finished";
}

void Controller::GetZphasePos(int addr)
{
    m_mutex.lock();
    emit sendReadRequestSignal(addr,4032);
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

}
