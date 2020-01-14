#include <QCoreApplication>
#include <QDebug>
#include <iostream>
#include "controller.h"
int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    qDebug() << "Program Start\nusing Serial:";
    qDebug()<<"card:"<<argv[1]<<" modbus:"<<argv[2];
    Controller controller(argv[1], argv[2]);
    //controller.simpleOperation();
    controller.reset();
    qDebug()<<"enter qt event loop";
    return a.exec();
}
