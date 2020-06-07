#include <vector>
#include <iostream>
#include "poseestimation2d2d.h"

using namespace cv;
PoseEstimation::PoseEstimation()
{

}
static void find_feature_matches(const Mat& img_1, const Mat& img_2, std::vector<KeyPoint>& keypoints_1,  std::vector<KeyPoint>& keypoints_2, std::vector< DMatch >& matches)
{
    //-- 初始化
    Mat descriptors_1, descriptors_2;
    // used in OpenCV3
    Ptr<FeatureDetector> detector = ORB::create();
    Ptr<DescriptorExtractor> descriptor = ORB::create();
    // use this if you are in OpenCV2
    // Ptr<FeatureDetector> detector = FeatureDetector::create ( "ORB" );
    // Ptr<DescriptorExtractor> descriptor = DescriptorExtractor::create ( "ORB" );
    Ptr<DescriptorMatcher> matcher = DescriptorMatcher::create("BruteForce-Hamming");
    //-- 第一步:检测 Oriented FAST 角点位置
    detector->detect(img_1, keypoints_1);
    detector->detect(img_2, keypoints_2);

    //-- 第二步:根据角点位置计算 BRIEF 描述子
    descriptor->compute(img_1, keypoints_1, descriptors_1);
    descriptor->compute(img_2, keypoints_2, descriptors_2);

    //-- 第三步:对两幅图像中的BRIEF描述子进行匹配，使用 Hamming 距离
    std::vector<DMatch> match;
    //BFMatcher matcher ( NORM_HAMMING );
    matcher->match(descriptors_1, descriptors_2, match);

    //-- 第四步:匹配点对筛选
    double min_dist = 10000, max_dist = 0;

    //找出所有匹配之间的最小距离和最大距离, 即是最相似的和最不相似的两组点之间的距离
    for (int i = 0; i < descriptors_1.rows; i++)
    {
        double dist = match[i].distance;
        if (dist < min_dist) min_dist = dist;
        if (dist > max_dist) max_dist = dist;
    }

    printf("-- Max dist : %f \n", max_dist);
    printf("-- Min dist : %f \n", min_dist);

    //当描述子之间的距离大于两倍的最小距离时,即认为匹配有误.但有时候最小距离会非常小,设置一个经验值30作为下限.
    for (int i = 0; i < descriptors_1.rows; i++)
    {
        if (match[i].distance <= max(2 * min_dist, 30.0))
        {
            matches.push_back(match[i]);
        }
    }
}
static void pose_estimation_2d2d(std::vector<KeyPoint> keypoints_1, std::vector<KeyPoint> keypoints_2, std::vector< DMatch > matches, Mat& R, Mat& t)
{
    // 相机内参,TUM Freiburg2
    Mat K = (Mat_<double>(3, 3) << 520.9, 0, 325.1, 0, 521.0, 249.7, 0, 0, 1);

    //-- 把匹配点转换为vector<Point2f>的形式
    std::vector<Point2f> points1;
    std::vector<Point2f> points2;

    for (int i = 0; i < (int)matches.size(); i++)
    {
        points1.push_back(keypoints_1[matches[i].queryIdx].pt);
        points2.push_back(keypoints_2[matches[i].trainIdx].pt);
    }

    //-- 计算基础矩阵
    Mat fundamental_matrix;
    fundamental_matrix = findFundamentalMat(points1, points2, CV_FM_8POINT);
    std::cout << "fundamental_matrix is " << std::endl << fundamental_matrix << std::endl;

    //-- 计算本质矩阵
    Point2d principal_point(325.1, 249.7);	//相机光心, TUM dataset标定值
    double focal_length = 521;			//相机焦距, TUM dataset标定值
    Mat essential_matrix;
    essential_matrix = findEssentialMat(points1, points2, focal_length, principal_point);
    std::cout << "essential_matrix is " << std::endl << essential_matrix << std::endl;

    //-- 计算单应矩阵
    Mat homography_matrix;
    homography_matrix = findHomography(points1, points2, RANSAC, 3);
    std::cout << "homography_matrix is " << std::endl << homography_matrix << std::endl;

    //-- 从本质矩阵中恢复旋转和平移信息.
    recoverPose(essential_matrix, points1, points2, R, t, focal_length, principal_point);
    std::cout << "R is " << std::endl << R << std::endl;
    std::cout << "t is " << std::endl << t << std::endl;

}
QVector<cv::Mat> PoseEstimation::GetPose2d2d(cv::Mat& img1, cv::Mat& img2)
{
    std::vector<cv::KeyPoint> keypoints_1, keypoints_2;
    std::vector<cv::DMatch> matches;
    find_feature_matches(img1, img2, keypoints_1, keypoints_2, matches);
    std::cout << "一共找到了" << matches.size() << "组匹配点" << std::endl;

    //-- 估计两张图像间运动
    cv::Mat R, t;
    pose_estimation_2d2d(keypoints_1, keypoints_2, matches, R, t);
    QVector<cv::Mat> res;
    res.push_back(R);
    res.push_back(t);
    return res;
}