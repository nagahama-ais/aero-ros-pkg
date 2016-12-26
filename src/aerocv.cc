#include "aero_std/aerocv.hh"
#include "aero_std/time.h"

using namespace aero;
using namespace aerocv;

//////////////////////////////////////////////////
std::vector<objectarea> aero::aerocv::DetectObjectnessArea
(pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud,
 cv::Mat &img, cv::Vec3b _env_color, bool _debug_view)
{
  auto begin = aero::time::now();

  std::vector<objectarea> scene;
  int w_scale = img.cols / cloud->width;
  int h_scale = img.rows / cloud->height;

  // get normal
  pcl::NormalEstimation<pcl::PointXYZRGB, pcl::Normal> ne;
  ne.setInputCloud(cloud);
  pcl::search::KdTree<pcl::PointXYZRGB>::Ptr tree
    (new pcl::search::KdTree<pcl::PointXYZRGB>);
  ne.setSearchMethod(tree);
  ne.setRadiusSearch(0.03);
  pcl::PointCloud<pcl::Normal>::Ptr normal(new pcl::PointCloud<pcl::Normal>);
  ne.compute(*normal);

  // cluster with region growing
  pcl::RegionGrowing<pcl::PointXYZRGB, pcl::Normal> reg;
  reg.setMinClusterSize(50);
  reg.setMaxClusterSize(1000000);
  reg.setSearchMethod(tree);
  reg.setNumberOfNeighbours(30);
  reg.setInputCloud(cloud);
  reg.setInputNormals(normal);
  reg.setSmoothnessThreshold(3.0 / 180.0 * M_PI);
  reg.setCurvatureThreshold(1.0);
  std::vector<pcl::PointIndices> clusters;
  reg.extract(clusters);

  // evaluate whether cluster is within expected range
  // this operation removes floors and walls
  float radius_threshold = 1.2; // meter
  for (auto it = clusters.begin(); it != clusters.end(); ) {
    Eigen::Vector3f center = {0.0, 0.0, 0.0};

    for (auto pit = it->indices.begin(); pit != it->indices.end(); ++pit)
      center +=
        Eigen::Vector3f(cloud->points[*pit].x, cloud->points[*pit].y,
                        cloud->points[*pit].z);

    center /= it->indices.size();
    if (center.norm() > radius_threshold) {
      it = clusters.erase(it);
      continue;
    }

    // add object to scene
    objectarea obj;
    obj.indices3d.assign(it->indices.begin(), it->indices.end());
    obj.visible3d = true;
    obj.center3d = center;
    scene.push_back(obj);
    ++it;
  }

  // find indices without a cluster label
  std::vector<uchar> indices_without_label(cloud->points.size(), 255);
  for (auto it = clusters.begin(); it != clusters.end(); ++it)
    for (auto pit = it->indices.begin(); pit != it->indices.end(); ++pit)
      indices_without_label[*pit] = 0;

  // binary cluster non-labeled regions
  cv::Mat binary_img(cloud->height, cloud->width, CV_8U);
  int at = 0;
  for (unsigned int i = 0; i < binary_img.rows; ++i)
    for (unsigned int j = 0; j < binary_img.cols; ++j)
      binary_img.at<uchar>(i, j) = indices_without_label[at++];
  cv::Mat labeled_image;
  cv::Mat stats;
  cv::Mat centroids;
  int n_labels =
    cv::connectedComponentsWithStats(binary_img, labeled_image, stats, centroids);

  // analyze non-labeled regions
  int noise_threshold = 50; // pixels
  int outmost_label = -1;
  int max_outmost_area = 0;
  for (int k = 1; k < n_labels; ++k) {
    int *param = stats.ptr<int>(k);
    int x = param[cv::ConnectedComponentsTypes::CC_STAT_LEFT];
    int y = param[cv::ConnectedComponentsTypes::CC_STAT_TOP];
    int height = param[cv::ConnectedComponentsTypes::CC_STAT_HEIGHT];
    int width = param[cv::ConnectedComponentsTypes::CC_STAT_WIDTH];

    // remove noise
    if (width * height < noise_threshold) continue;
    // remove region adjacent to the edge of image
    // this removes floor, wall, and edge NaN noises all together
    // but will also remove some connected regions as well
    if (x == 0 || (x + width) == cloud->width ||
        y == 0 || (y + height) == cloud->height) {
      if (param[cv::ConnectedComponentsTypes::CC_STAT_AREA] > max_outmost_area) {
        outmost_label = k; // keep track of largest edge region
        max_outmost_area = param[cv::ConnectedComponentsTypes::CC_STAT_AREA];
      }
      continue;
    }

    // add object to scene
    objectarea obj;
    obj.indices3d.reserve(param[cv::ConnectedComponentsTypes::CC_STAT_AREA]);
    for (unsigned int i = 0; i < height; ++i)
      for (unsigned int j = 0; j < width; ++j)
        if (labeled_image.at<int>(y + i, x + j) == k)
          obj.indices3d.push_back
            (static_cast<int>((y + i) * labeled_image.cols + x + j));
    obj.bounds2d =
      cv::Rect(x*w_scale, y*h_scale, width*w_scale, height*h_scale);
    obj.visible3d = false;
    scene.push_back(obj);
  }

  // largest edge region usually has a complex over connection
  // break region apart by suppressing countour regions
  // why this works: connections are usually due to undefined object edges
  // the suppression eliminates such undefined edges
  cv::Mat outmost(cloud->height, cloud->width, CV_8U);
  if (outmost_label > 0) {
    int at = 0;
    for (unsigned int i = 0; i < labeled_image.rows; ++i)
      for (unsigned int j = 0; j < labeled_image.cols; ++j)
        if (labeled_image.at<int>(i, j) == outmost_label)
          outmost.at<uchar>(i, j) = 255;
        else
          outmost.at<uchar>(i, j) = 0;

    // find contours destroyes image, so clone
    std::vector<std::vector<cv::Point> > contours;
    cv::findContours(outmost.clone(), contours, CV_RETR_LIST, CV_CHAIN_APPROX_NONE);

    // break region apart by suppressing contours
    int suppress_margin = 6;
    for (auto contour = contours.begin(); contour != contours.end(); ++contour)
      cv::polylines(outmost, *contour, true, cv::Scalar(0), suppress_margin);

    // get new clusters
    cv::Mat labeled_image;
    cv::Mat stats;
    cv::Mat centroids;
    int n_labels =
      cv::connectedComponentsWithStats(outmost, labeled_image, stats, centroids, 4);

    // add clusters to list if not too small
    // because, clusters were suppressed, use a lower noise threshold
    // the upper threshold will remove backgrounds
    int lower_noise_threshold = 30;
    int upper_noise_threshold = 300;
    for (int k = 1; k < n_labels; ++k) {
      int *param = stats.ptr<int>(k);
      int x = param[cv::ConnectedComponentsTypes::CC_STAT_LEFT];
      int y = param[cv::ConnectedComponentsTypes::CC_STAT_TOP];
      int height = param[cv::ConnectedComponentsTypes::CC_STAT_HEIGHT];
      int width = param[cv::ConnectedComponentsTypes::CC_STAT_WIDTH];

      // remove noise
      if (width * height < lower_noise_threshold) continue;

      // big clusters nearby camera are likely to be objects, so do not remove
      // when the bound of background clusters covers the whole image,
      // the cluster center is the center of image
      // the distance threshold is set so that such clusters are removed
      if (y + 0.5 * height <= 0.5 * cloud->height + 1)
        if (param[cv::ConnectedComponentsTypes::CC_STAT_AREA]
            > upper_noise_threshold)
          continue;

      objectarea obj;
      obj.indices3d.reserve(param[cv::ConnectedComponentsTypes::CC_STAT_AREA]);

      // get current cluster to check further division
      cv::Mat blob = cv::Mat::zeros(height, width, CV_8U);
      for (unsigned int i = 0; i < height; ++i)
        for (unsigned int j = 0; j < width; ++j)
          if (labeled_image.at<int>(y + i, x + j) == k) {
            blob.at<uchar>(i, j) = 255;
            obj.indices3d.push_back
              (static_cast<int>((y + i) * labeled_image.cols + x + j));
          }

      // conduct further division
      cv::Mat divs;
      cv::Mat divstats;
      cv::Mat divcentroids;
      int n_div =
        aero::aerocv::cutComponentsWithStats(
            blob, divs, divstats, divcentroids, _debug_view);

      // if no further division
      if (n_div == 1) {
        obj.bounds2d =
          cv::Rect(x*w_scale, y*h_scale, width*w_scale, height*h_scale);
        obj.visible3d = false;
        scene.push_back(obj);
        continue;
      }

      // if there was further division
      for (int l = 1; l < n_div; ++l) {
        int *bounds = divstats.ptr<int>(l);
        int dx = bounds[cv::ConnectedComponentsTypes::CC_STAT_LEFT];
        int dy = bounds[cv::ConnectedComponentsTypes::CC_STAT_TOP];
        int dheight = bounds[cv::ConnectedComponentsTypes::CC_STAT_HEIGHT];
        int dwidth = bounds[cv::ConnectedComponentsTypes::CC_STAT_WIDTH];

        objectarea dobj;
        dobj.indices3d.reserve(bounds[cv::ConnectedComponentsTypes::CC_STAT_AREA]);
        for (unsigned int i = 0; i < dheight; ++i)
          for (unsigned int j = 0; j < dwidth; ++j)
            if (divs.at<int>(dy + i, dx + j) == k)
              dobj.indices3d.push_back
                (static_cast<int>((y + dy + i) * labeled_image.cols + x + dx + j));
        dobj.bounds2d =
          cv::Rect((x+dx)*w_scale, (y+dy)*h_scale, dwidth*w_scale, dheight*h_scale);
        dobj.visible3d = false;
        scene.push_back(dobj);
      }
    }
  }

  // analyze bounds, corners, color properties of RGS regions
  for (auto it = clusters.begin(); it != clusters.end(); ) {
    auto obj = scene.begin() + static_cast<int>(it - clusters.begin());

    std::vector<uchar> cluster1d(cloud->points.size(), 0);
    std::vector<cv::Vec3b> colors1d(it->indices.size());
    int color1d_idx = 0;
    for (auto pit = it->indices.begin(); pit != it->indices.end(); ++pit) {
      cluster1d[*pit] = 255;
      // in the meantime, get color as well
      uint32_t rgb = *reinterpret_cast<int*>(&cloud->points[*pit].rgb);
      colors1d[color1d_idx++] = cv::Vec3b
        ((rgb) & 0x0000ff, (rgb >> 8) & 0x0000ff, (rgb >> 16) & 0x0000ff);
    }

    // get color information of cluster
    int quantize_amount = 4;
    std::vector<cv::Vec3b> dominant_colors(quantize_amount);
    aero::aerocv::medianCut(colors1d, 0, 0, dominant_colors);
    // check how many colors match environment color information
    float distance_threshold = 20;
    int expected_matches = 3;
    int matches = 0;
    for (auto c = dominant_colors.begin(); c != dominant_colors.end(); ++c)
      if (aero::aerocv::distance(aero::aerocv::rgb2lab(*c),
                                 aero::aerocv::rgb2lab(_env_color))
          < distance_threshold)
        ++matches;
    if (matches >= expected_matches) { // if likely environment
      it = clusters.erase(it);
      scene.erase(obj); // remove environment object(e.g. table) from scene
      if (_debug_view)
        aero::aerocv::drawPalette(dominant_colors, _env_color);
      continue;
    }

    // from color palette, get best matched color names
    aero::aerocv::getColorNames
      (dominant_colors, obj->properties.colors, aero::aerocv::colorMap9);

    // create cv::Mat from cluster1d
    cv::Mat cluster(cloud->height, cloud->width, CV_8U);
    int at = 0;
    for (unsigned int i = 0; i < cluster.rows; ++i)
      for (unsigned int j = 0; j < cluster.cols; ++j)
        cluster.at<uchar>(i, j) = cluster1d[at++];

    // get bounding box of cluster
    auto bb = cv::boundingRect(cluster);
    obj->bounds2d =
      cv::Rect(bb.x*w_scale, bb.y*h_scale, bb.width*w_scale, bb.height*h_scale);

    // get additional infos

    // get corner2d
    auto corners = aero::aerocv::getCornersInBoundingBox(cluster, bb);
    obj->corners2d =
      {cv::Point2f(corners.at(0).x*w_scale, corners.at(0).y*h_scale),
       cv::Point2f(corners.at(1).x*w_scale, corners.at(1).y*h_scale),
       cv::Point2f(corners.at(2).x*w_scale, corners.at(2).y*h_scale),
       cv::Point2f(corners.at(3).x*w_scale, corners.at(3).y*h_scale)};

    // get width3d and height3d from corners
    int tl = (bb.y + corners.at(0).y) * cloud->width + bb.x + corners.at(0).x;
    Eigen::Vector3f tl_pos
      (cloud->points[tl].x, cloud->points[tl].y, cloud->points[tl].z);
    int tr = (bb.y + corners.at(1).y) * cloud->width + bb.x + corners.at(1).x;
    Eigen::Vector3f tr_pos
      (cloud->points[tr].x, cloud->points[tr].y, cloud->points[tr].z);
    int br = (bb.y + corners.at(2).y) * cloud->width + bb.x + corners.at(2).x;
    Eigen::Vector3f br_pos
      (cloud->points[br].x, cloud->points[br].y, cloud->points[br].z);
    int bl = (bb.y + corners.at(3).y) * cloud->width + bb.x + corners.at(3).x;
    Eigen::Vector3f bl_pos
      (cloud->points[bl].x, cloud->points[bl].y, cloud->points[bl].z);
    obj->width3d = (tr_pos - tl_pos).norm();
    obj->height3d = (br_pos - bl_pos).norm();

    ++it;
  }

  auto end = aero::time::now();

  std::cout << "detection time " << aero::time::ms(end - begin) << std::endl;

  // view results if debug mode is true

  if (_debug_view) {
    // draw bounds2d

    for (auto it = scene.begin(); it < scene.begin() + clusters.size(); ++it) {
      cv::rectangle(img, it->bounds2d, cv::Scalar(0, 255, 0), 2);

      // write color properties info
      std::string text = "";
      for (auto c = it->properties.colors.begin();
           c != it->properties.colors.end(); ++c) {
        std::ostringstream val;
        val << std::setprecision(2) << c->second;
        text += c->first + "(" + val.str() + ") ";
      }
      cv::putText(img, text,
                  cv::Point(it->bounds2d.x, it->bounds2d.y + it->bounds2d.height),
                  cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255,255,255), 1.0);
    }

    for (auto it = scene.begin() + clusters.size(); it != scene.end(); ++it)
      cv::rectangle(img, it->bounds2d, cv::Scalar(255, 0, 0), 2);

    // show results

    cv::namedWindow("mid1", CV_WINDOW_NORMAL);
    cv::resizeWindow("mid1", 640, 480);
    cv::imshow("mid1", binary_img);
    cv::waitKey(100);

    if (outmost_label > 0) {
      cv::namedWindow("mid2", CV_WINDOW_NORMAL);
      cv::resizeWindow("mid2", 640, 480);
      cv::imshow("mid2", outmost);
      cv::waitKey(100);
    }

    cv::namedWindow("result", CV_WINDOW_NORMAL);
    cv::resizeWindow("result", 640, 480);
    cv::imshow("result", img);
    cv::waitKey(100);
  }

  return scene;
}
