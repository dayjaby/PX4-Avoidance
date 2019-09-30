#include "local_planner/local_planner.h"

#include "avoidance/common.h"
#include "local_planner/star_planner.h"
#include "local_planner/tree_node.h"

#include <sensor_msgs/image_encodings.h>

namespace avoidance {

LocalPlanner::LocalPlanner() : star_planner_(new StarPlanner()) {}

LocalPlanner::~LocalPlanner() {}

// update UAV pose
void LocalPlanner::setState(const Eigen::Vector3f& pos, const Eigen::Vector3f& vel, const Eigen::Quaternionf& q) {
  position_ = pos;
  velocity_ = vel;
  yaw_fcu_frame_deg_ = getYawFromQuaternion(q);
  pitch_fcu_frame_deg_ = getPitchFromQuaternion(q);
  star_planner_->setPose(position_, velocity_, q);
}

// set parameters changed by dynamic rconfigure
void LocalPlanner::dynamicReconfigureSetParams(avoidance::LocalPlannerNodeConfig& config, uint32_t level) {
  max_sensor_range_ = static_cast<float>(config.max_sensor_range_);
  cost_params_.pitch_cost_param = config.pitch_cost_param_;
  cost_params_.yaw_cost_param = config.yaw_cost_param_;
  cost_params_.velocity_cost_param = config.velocity_cost_param_;
  cost_params_.obstacle_cost_param = config.obstacle_cost_param_;
  max_point_age_s_ = static_cast<float>(config.max_point_age_s_);
  min_num_points_per_cell_ = config.min_num_points_per_cell_;
  min_sensor_range_ = static_cast<float>(config.min_sensor_range_);
  timeout_startup_ = config.timeout_startup_;
  timeout_critical_ = config.timeout_critical_;
  timeout_termination_ = config.timeout_termination_;
  children_per_node_ = config.children_per_node_;
  n_expanded_nodes_ = config.n_expanded_nodes_;
  smoothing_margin_degrees_ = static_cast<float>(config.smoothing_margin_degrees_);
  tree_node_duration_ = static_cast<float>(config.tree_node_duration_);

  if (getGoal().z() != config.goal_z_param) {
    auto goal = getGoal();
    goal.z() = config.goal_z_param;
    setGoal(goal);
  }

  star_planner_->dynamicReconfigureSetStarParams(config, level);

  ROS_DEBUG("\033[0;35m[OA] Dynamic reconfigure call \033[0m");
}

void LocalPlanner::setGoal(const Eigen::Vector3f& goal) {
  goal_ = goal;

  ROS_INFO("===== Set Goal ======: [%f, %f, %f].", goal_.x(), goal_.y(), goal_.z());
  applyGoal();
}
void LocalPlanner::setPreviousGoal(const Eigen::Vector3f& prev_goal) { prev_goal_ = prev_goal; }

void LocalPlanner::setFOV(int i, const FOV& fov) {
  if (i < fov_fcu_frame_.size()) {
    fov_fcu_frame_[i] = fov;
  } else {
    fov_fcu_frame_.push_back(fov);
  }
}

Eigen::Vector3f LocalPlanner::getGoal() const { return goal_; }

void LocalPlanner::applyGoal() { star_planner_->setGoal(goal_); }

void LocalPlanner::runPlanner() {
  ROS_INFO("\033[1;35m[OA] Planning started, using %i cameras\n \033[0m",
           static_cast<int>(original_cloud_vector_.size()));

  float elapsed_since_last_processing = static_cast<float>((ros::Time::now() - last_pointcloud_process_time_).toSec());
  processPointcloud(final_cloud_, original_cloud_vector_, fov_fcu_frame_, yaw_fcu_frame_deg_, pitch_fcu_frame_deg_,
                    position_, min_sensor_range_, max_sensor_range_, max_point_age_s_, elapsed_since_last_processing,
                    min_num_points_per_cell_);
  last_pointcloud_process_time_ = ros::Time::now();

  determineStrategy();
}

void LocalPlanner::create2DObstacleRepresentation(const bool send_to_fcu) {
  // construct histogram if it is needed
  // or if it is required by the FCU
  Histogram new_histogram = Histogram(ALPHA_RES);
  to_fcu_histogram_.setZero();
  generateHistogramHACK(new_histogram, final_cloud_, position_, 2.5f);  // HACK: hard-code 2.5m outward projection

  if (send_to_fcu) {
    compressHistogramElevation(to_fcu_histogram_, new_histogram, position_);
    updateObstacleDistanceMsg(to_fcu_histogram_);
  }
  polar_histogram_ = new_histogram;

  // generate histogram image for logging
  generateHistogramImage(polar_histogram_);
}

void LocalPlanner::generateHistogramImage(Histogram& histogram) {
  histogram_image_data_.clear();
  histogram_image_data_.reserve(GRID_LENGTH_E * GRID_LENGTH_Z);

  // fill image data
  for (int e = GRID_LENGTH_E - 1; e >= 0; e--) {
    for (int z = 0; z < GRID_LENGTH_Z; z++) {
      float dist = histogram.get_dist(e, z);
      float depth_val = dist > 0.01f ? 255.f - 255.f * dist / max_sensor_range_ : 0.f;
      histogram_image_data_.push_back((int)std::max(0.0f, std::min(255.f, depth_val)));
    }
  }
}

void LocalPlanner::determineStrategy() {
  // clear cost image
  cost_image_data_.clear();
  cost_image_data_.resize(3 * GRID_LENGTH_E * GRID_LENGTH_Z, 0);

  create2DObstacleRepresentation(px4_.param_mpc_col_prev_d > 0.f);

  // calculate the vehicle projected position on the line between the previous and current goal
  Eigen::Vector2f u_prev_to_goal = (goal_ - prev_goal_).head<2>().normalized();
  Eigen::Vector2f prev_to_pos = (position_ - prev_goal_).head<2>();
  closest_pt_.head<2>() = prev_goal_.head<2>() + (u_prev_to_goal * u_prev_to_goal.dot(prev_to_pos));
  closest_pt_.z() = goal_.z();

  // if the vehicle is less than the cruise speed away from the line, set the projection point to the goal such that
  // the cost function doesn't pull the vehicle towards the line
  if ((position_ - closest_pt_).head<2>().norm() < px4_.param_mpc_xy_cruise) {
    closest_pt_ = goal_;
  }

  if (!polar_histogram_.isEmpty()) {
    getCostMatrix(polar_histogram_, goal_, position_, velocity_, cost_params_, smoothing_margin_degrees_, closest_pt_,
                  max_sensor_range_, min_sensor_range_, cost_matrix_, cost_image_data_);

    simulation_limits lims;
    setDefaultPx4Parameters();  // TODO: remove but make sure they're set!
    lims.max_z_velocity = px4_.param_mpc_z_vel_max_up;
    lims.min_z_velocity = -1.0f * px4_.param_mpc_z_vel_max_dn;
    lims.max_xy_velocity_norm =
        std::min(getMaxSpeed(px4_.param_mpc_jerk_max, px4_.param_mpc_acc_hor, max_sensor_range_,
                             px4_.param_mpc_xy_cruise, mission_item_speed_),
                 getMaxSpeed(px4_.param_mpc_jerk_max, px4_.param_mpc_acc_hor, (goal_ - position_).norm(),
                             px4_.param_mpc_xy_cruise, mission_item_speed_));
    lims.max_acceleration_norm = px4_.param_mpc_acc_hor;
    lims.max_jerk_norm = px4_.param_mpc_jerk_max;
    star_planner_->setParams(cost_params_, lims, px4_.param_nav_acc_rad);
    star_planner_->setPointcloud(final_cloud_);
    star_planner_->setClosestPointOnLine(closest_pt_);

    // build search tree
    star_planner_->buildLookAheadTree();
    last_path_time_ = ros::Time::now();
  }
}

void LocalPlanner::updateObstacleDistanceMsg(Histogram hist) {
  sensor_msgs::LaserScan msg = {};
  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "local_origin";
  msg.angle_increment = static_cast<double>(ALPHA_RES) * M_PI / 180.0;
  msg.range_min = min_sensor_range_;
  msg.range_max = max_sensor_range_;
  msg.ranges.reserve(GRID_LENGTH_Z);

  for (int i = 0; i < GRID_LENGTH_Z; ++i) {
    // turn idxs 180 degress to point to local north instead of south
    int j = (i + GRID_LENGTH_Z / 2) % GRID_LENGTH_Z;
    float dist = hist.get_dist(0, j);

    // is bin inside FOV?
    if (histogramIndexYawInsideFOV(fov_fcu_frame_, j, position_, yaw_fcu_frame_deg_)) {
      msg.ranges.push_back(dist > min_sensor_range_ ? dist : max_sensor_range_ + 1.0f);
    } else {
      msg.ranges.push_back(NAN);
    }
  }

  distance_data_ = msg;
}

void LocalPlanner::updateObstacleDistanceMsg() {
  sensor_msgs::LaserScan msg = {};
  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "local_origin";
  msg.angle_increment = static_cast<double>(ALPHA_RES) * M_PI / 180.0;
  msg.range_min = min_sensor_range_;
  msg.range_max = max_sensor_range_;

  distance_data_ = msg;
}

Eigen::Vector3f LocalPlanner::getPosition() const { return position_; }

pcl::PointCloud<pcl::PointXYZI> LocalPlanner::getPointcloud() const {
  pcl::PointCloud<pcl::PointXYZI> ret;
  std::vector<std::pair<std::array<float, 3>, float>> local_cloud = final_cloud_.getAllPoints();
  for (const auto& p : local_cloud) {
    ret.push_back(toXYZI(p.first[0], p.first[1], p.first[2], p.second));
  }
  ret.header.frame_id = "/local_origin";
  return ret;
}

void LocalPlanner::setDefaultPx4Parameters() {
  px4_.param_mpc_auto_mode = 1;
  px4_.param_mpc_jerk_min = 8.f;
  px4_.param_mpc_jerk_max = 20.f;
  px4_.param_acc_up_max = 10.f;
  px4_.param_mpc_z_vel_max_up = 3.f;
  px4_.param_mpc_acc_down_max = 10.f;
  px4_.param_mpc_z_vel_max_dn = 1.f;
  px4_.param_mpc_acc_hor = 5.f;
  px4_.param_mpc_xy_cruise = 3.f;
  px4_.param_mpc_tko_speed = 1.f;
  px4_.param_mpc_land_speed = 0.7f;
  px4_.param_mpc_col_prev_d = 4.f;
  px4_.param_nav_acc_rad = 2.f;
}

void LocalPlanner::getTree(std::vector<TreeNode>& tree, std::vector<int>& closed_set,
                           std::vector<Eigen::Vector3f>& path_node_setpoints) const {
  tree = star_planner_->tree_;
  closed_set = star_planner_->closed_set_;
  path_node_setpoints = star_planner_->path_node_setpoints_;
}

void LocalPlanner::getObstacleDistanceData(sensor_msgs::LaserScan& obstacle_distance) {
  obstacle_distance = distance_data_;
}

avoidanceOutput LocalPlanner::getAvoidanceOutput() const {
  avoidanceOutput out;
  out.cruise_velocity = getMaxSpeed(px4_.param_mpc_jerk_max, px4_.param_mpc_acc_hor, max_sensor_range_,
                                    px4_.param_mpc_xy_cruise, mission_item_speed_);
  out.last_path_time = last_path_time_;
  out.tree_node_duration = tree_node_duration_;
  out.path_node_setpoints = star_planner_->path_node_setpoints_;
  return out;
}
}
