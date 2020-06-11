#include <QDebug>
#include <windows.h>
#include <algorithm>
#include "imgprocessthread.h"
#include "MVCamera.h"
#include "controller.h"

static void start_camera()
{
    MVCamera::Init();
    MVCamera::Play();
    MVCamera::SetExposureTime(false, 20000);
    MVCamera::SetLargeResolution(true);
}

ImgProcessThread::ImgProcessThread(int _method)
{
    connect(this, &ImgProcessThread::finished, this, &QObject::deleteLater);
    if(_method==0)
    {
        qDebug()<<"using laptop camera to get image";
        GetFrame=[](cv::Mat& frame)->bool
        {
            static cv::VideoCapture cap(0);
            return cap.read(frame);
        };
    }
    else if(_method==1)
    {
        qDebug()<<"using industry camera to get image";
        start_camera();
        GetFrame=&MVCamera::GetFrame;
    }
    else
    {
        qDebug()<<"no this method";
        exit(1);
    }
}
void ImgProcessThread::run()
{
    cv::Mat srcImg;
    cv::Mat preImg;
    GetFrame(srcImg);
    while(GetFrame(srcImg))
    {
        std::reverse(srcImg.data,srcImg.data+srcImg.cols*srcImg.rows*3);
        if (!srcImg.isContinuous())
        {
            qDebug()<<"img not continues!!";
            srcImg = srcImg.clone();
        }
        cv::Mat temp=srcImg.clone();
        if(!preImg.empty())
        {
            auto mats=estimation_module.GetPose2d2d(preImg,srcImg);
            frame_mutex.lock();
            FrameRotation=mats[0];
            FrameTranslation=mats[1];
            frame_mutex.unlock();
#if __SHOW_SRC__
            cv::imshow("pre",preImg);
#endif
        }
#if __SHOW_SRC__
        cv::imshow("src",srcImg);
        cv::waitKey(2);
#endif
        preImg=temp;
    }
    qDebug()<<"image thread finished";
}
