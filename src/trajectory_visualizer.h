#ifndef TRAJECTORY_VISUALIZER_H
#define TRAJECTORY_VISUALIZER_H

#include <ros/ros.h>
#include <nav_msgs/Path.h>
#include <visualization_msgs/Marker.h>

#include <iostream>

#include "trajectory_selector.h"
//#include "trajectory_selector_utils.h"

class TrajectoryVisualizer {
public:
	
	TrajectoryVisualizer() {};

	void initialize(TrajectorySelector* trajectory_selector, ros::NodeHandle & nh, size_t* best_traj_index, double const& final_time) {
		this->trajectory_selector = trajectory_selector;
		this->nh = nh;
		this->best_traj_index = best_traj_index;
		this->final_time = final_time;
		collision_probabilities.setZero();

		gaussian_pub = nh.advertise<visualization_msgs::Marker>( "gaussian_visualization", 0 );

		std::cout << "Visualizer was created with best_traj_index and final_time " << best_traj_index << " " << final_time << std::endl;

		initializeDrawingPaths();
		createSamplingTimeVector();
	};

  void TestVisualizer();
  void createSamplingTimeVector();

  void initializeDrawingPaths();

  void drawAll();
  void drawGaussianPropagation(int id, Vector3 position, Vector3 sigma);
  void drawFinalStoppingPosition(int id, Vector3 position);
  void drawCollisionIndicator(int const& id, Vector3 const& position, double const& collision_prob);
  void setCollisionProbabilities(Eigen::Matrix<Scalar, 25, 1> const& collision_probabilities) {
  	this->collision_probabilities = collision_probabilities;
  }
  void drawDebugPoints();
  

private:

	void NormalizeCollisions();

	std::string drawing_frame = "hummingbird/vi_sensor/camera_depth_optical_center_link";

	ros::NodeHandle nh;
	ros::Publisher gaussian_pub;
	std::vector<ros::Publisher> action_paths_pubs;

  TrajectorySelector* trajectory_selector;

  Eigen::Matrix<Scalar, Eigen::Dynamic, 1> sampling_time_vector;
  size_t num_samples;
  double start_time = 0;
  double final_time;

  size_t* best_traj_index;

  Eigen::Matrix<Scalar, 25, 1> collision_probabilities;
  Eigen::Matrix<Scalar, 25, 1> normalized_collision_probabilities;
  
};

#endif
