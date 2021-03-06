#include "laser_scan_collision_evaluator.h"


void LaserScanCollisionEvaluator::UpdatePointCloudPtr(pcl::PointCloud<pcl::PointXYZ>::Ptr const& xyz_cloud_new) {
	//auto t1 = std::chrono::high_resolution_clock::now();
	xyz_cloud_ptr = xyz_cloud_new;
	// auto t2 = std::chrono::high_resolution_clock::now();
	// std::cout << "Converting and saving the point cloud took "
 //      << std::chrono::duration_cast<std::chrono::microseconds>(t2-t1).count()
 //      << " microseconds\n"; 
}



double LaserScanCollisionEvaluator::computeProbabilityOfCollisionOnePosition(Vector3 const& robot_position, Vector3 const& sigma_robot_position) {
	//std::cout << "Robot position given" << robot_position << std::endl;
	//std::cout << "sigma_robot_position given" << sigma_robot_position << std::endl;
	double closest_distance = 100;
	double current_distance;

	double probability_no_collision = 1.0;
	double probability_of_collision_one_return;


	Vector3 total_sigma = sigma_robot_position + sigma_depth_point;
  	Vector3 inverse_total_sigma = Vector3(1/total_sigma(0), 1/total_sigma(1), 1/total_sigma(2));
  	double volume = 0.2*0.267; // 4/3*pi*r^3, with r=0.4 as first guess
  	double denominator = std::sqrt( 248.05021344239853*(total_sigma(0))*(total_sigma(1))*(total_sigma(2)) ); // coefficient is 2pi*2pi*2pi	
	
	if (xyz_cloud_ptr != nullptr) {

		auto point_cloud_iterator_begin = xyz_cloud_ptr->begin();
  		auto point_cloud_iterator_end = xyz_cloud_ptr->end();
  		for (auto point = point_cloud_iterator_begin; point != point_cloud_iterator_end; point++) {
  			Vector3 depth_position = Vector3(point->x, point->y, point->z);

  			current_distance = (depth_position - robot_position).norm();
  			if (current_distance < closest_distance) {
  				closest_distance = current_distance;
  			}

  			double exponent = -0.5*(robot_position - depth_position).transpose() * inverse_total_sigma.cwiseProduct(robot_position - depth_position);
  			probability_of_collision_one_return = volume / denominator * std::exp(exponent);
  			//std::cout << "probability_of_collision_one_return " << probability_of_collision_one_return << std::endl;
  			if (probability_of_collision_one_return < 0.02) {probability_of_collision_one_return=0.0;}
  			probability_no_collision = probability_no_collision * (1 - probability_of_collision_one_return);
  		}

  		// if (closest_distance < 0.5) {
  		// 	std::cout << "My closest distance was " << closest_distance << std::endl;
  		// 	std::cout << "probability_no_collision was " << probability_no_collision << std::endl;
  		// }

		return 1 - probability_no_collision;	
	}
	// Ptr was null
	return 0.0;
}

Eigen::Matrix<Scalar, 100, 3> LaserScanCollisionEvaluator::DebugPointsToDraw() {

	Eigen::Matrix<Scalar, 100, 3> points_to_draw;
	points_to_draw.setZero();

	if (xyz_cloud_ptr != nullptr) {

		auto point_cloud_iterator_begin = xyz_cloud_ptr->begin();
  		auto point_cloud_iterator_end = xyz_cloud_ptr->end();
  		int i = 0;
  		for (auto point = point_cloud_iterator_begin; point != point_cloud_iterator_end; point++) {
  			Vector3 depth_position = Vector3(point->x, point->y, point->z);
  			points_to_draw.row(i) = depth_position;
  			i++;
  		}
	}
	// Ptr was null
	return points_to_draw;

};