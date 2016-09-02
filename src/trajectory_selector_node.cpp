#include <ros/ros.h>
#include <nav_msgs/Path.h>
#include <visualization_msgs/Marker.h>
#include <sensor_msgs/PointCloud2.h>
#include <nav_msgs/Odometry.h>
//#include <geometry_msgs/PoseWithCovarianceStamped.h>
//#include <geometry_msgs/TwistWithCovarianceStamped.h>
#include <mavros_msgs/AttitudeTarget.h>
#include <nav_msgs/OccupancyGrid.h>
#include <sensor_msgs/PointCloud2.h>

#include "tf/tf.h"
#include <tf2_ros/transform_listener.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#include <pcl/filters/voxel_grid.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <mutex>
#include <cmath>
#include <time.h>
#include <stdlib.h>
#include <chrono>

#include "trajectory_selector.h"
#include "attitude_generator.h"
#include "trajectory_visualizer.h"

#include <quad_msgs/AttitudeYawRateCommand.h>
#include <quad_msgs/QuadDesiredState.h>
#include <quad_msgs/QuadStateEstimate.h>
#include <quad_msgs/ControllerFeedback.h>
#include <eigen_conversions/eigen_msg.h>
#include <std_msgs/Bool.h>

class TrajectorySelectorNode {
public:

	TrajectorySelectorNode() 
    : go_(false), copilot_is_in_feedthrough(false)
  {

		// Subscribers
    copilot_feedback_sub = nh.subscribe("/hummingbird/copilot/feedback", 10, &TrajectorySelectorNode::CopilotFeedbackCallback, this);
		gt_sub = nh.subscribe("/hummingbird/ground_truth/odometry", 1, &TrajectorySelectorNode::OnGT, this);
		//pose_sub = nh.subscribe("/hummingbird/ground_truth/pose", 1, &TrajectorySelectorNode::OnPose, this);
		//velocity_sub = nh.subscribe("/hummingbird/ground_truth/odometry/twist", 1, &TrajectorySelectorNode::OnVelocity, this);
		//waypoints_sub = nh.subscribe("/waypoint_list", 1, &TrajectorySelectorNode::OnWaypoints, this);
  	    //depth_image_sub = nh.subscribe("/flight/xtion_depth/points", 1, &TrajectorySelectorNode::OnDepthImage, this);
  	    global_goal_sub = nh.subscribe("/move_base_simple/goal", 1, &TrajectorySelectorNode::OnGlobalGoal, this);
  	    //value_grid_sub = nh.subscribe("/value_grid", 1, &TrajectorySelectorNode::OnValueGrid, this);
  	    laser_scan_sub = nh.subscribe("/hummingbird/vi_sensor/camera_depth/depth/points", 1, &TrajectorySelectorNode::OnScan, this);

  	    // Publishers
		carrot_pub = nh.advertise<visualization_msgs::Marker>( "carrot_marker", 0 );
		gaussian_pub = nh.advertise<visualization_msgs::Marker>( "gaussian_visualization", 0 );
		//attitude_thrust_pub = nh.advertise<mavros_msgs::AttitudeTarget>("/mavros/setpoint_raw/attitude", 1);
		attitude_thrust_pub = nh.advertise<quad_msgs::AttitudeYawRateCommand>("/hummingbird/attitude_yaw_rate_command", 1);
		desired_state_pub = nh.advertise<quad_msgs::QuadDesiredState>("/hummingbird/desired_state", 1);
		//attitude_setpoint_visualization_pub = nh.advertise<geometry_msgs::PoseStamped>("attitude_setpoint", 1);
    feedthrough_pub = nh.advertise<std_msgs::Bool>("/hummingbird/copilot/feedthrough", 1);

		// Initialization
		trajectory_selector.InitializeLibrary(final_time);

		trajectory_visualizer.initialize(&trajectory_selector, nh, &best_traj_index, final_time);
		tf_listener_ = std::make_shared<tf2_ros::TransformListener>(tf_buffer_);
		srand ( time(NULL) ); //initialize the random seed

		ROS_INFO("Finished constructing the trajectory selector node, waiting for waypoints");
	}

	void SetThrustForLibrary(double thrust) {
		TrajectoryLibrary* trajectory_library_ptr = trajectory_selector.GetTrajectoryLibraryPtr();
		if (trajectory_library_ptr != nullptr) {
			trajectory_library_ptr->setThrust(thrust);
		}
	}

	geometry_msgs::TransformStamped GetTransformToWorld() {
		geometry_msgs::TransformStamped tf;
	    try {

	      tf = tf_buffer_.lookupTransform("world", "hummingbird/ortho_base_link", 
	                                    ros::Time(0), ros::Duration(1.0/30.0));
	    } catch (tf2::TransformException &ex) {
	      ROS_ERROR("%s", ex.what());
	      return tf;
	    }
	    return tf;
	}

	void ReactToSampledPointCloud() {

    if(!go_ || !copilot_is_in_feedthrough) return;

		Vector3 desired_acceleration;
		Vector3 desired_velocity;
		Vector3 desired_position;
    Vector3 desired_jerk;
    Scalar traj_t = (ros::Time::now() - last_depth).toSec();

		// uncomment for bearing control
		//SetGoalFromBearing();
		
		auto t1 = std::chrono::high_resolution_clock::now();
		trajectory_selector.computeBestEuclideanTrajectory(carrot_ortho_body_frame, 
                                                       best_traj_index, 
                                                       desired_acceleration, 
                                                       desired_velocity, 
                                                       desired_position,
                                                       desired_jerk,
                                                       traj_t);
		auto t2 = std::chrono::high_resolution_clock::now();
		//std::cout << "Computing best traj took "
    //	  << std::chrono::duration_cast<std::chrono::microseconds>(t2-t1).count()
    // 		<< " microseconds\n"; 

      	Eigen::Matrix<Scalar, 25, 1> collision_probabilities = trajectory_selector.getCollisionProbabilities();
		trajectory_visualizer.setCollisionProbabilities(collision_probabilities);

		Vector3 attitude_thrust_desired = attitude_generator.generateDesiredAttitudeThrust(desired_acceleration);

		SetThrustForLibrary(attitude_thrust_desired(2));

    //PublishAttitudeSetpoint(attitude_thrust_desired);
    PublishDesiredState(desired_acceleration, desired_velocity, desired_position, desired_jerk);
	}

private:

	void SetGoalFromBearing() {
		bool go;
		nh.param("go", go, false);
		if (go) {
			carrot_ortho_body_frame << 100, 0, 0;
		}
		else if (carrot_ortho_body_frame(0) == 100) {
			carrot_ortho_body_frame << 5, 0, 0;
		}
	}

	void UpdateTrajectoryLibraryRollPitch(double roll, double pitch) {
		TrajectoryLibrary* trajectory_library_ptr = trajectory_selector.GetTrajectoryLibraryPtr();
		if (trajectory_library_ptr != nullptr) {
			trajectory_library_ptr->setRollPitch(roll, pitch);
		}
	}

	void PublishOrthoBodyTransform(double roll, double pitch) {
    //ROS_INFO("Publishing ortho_base_link transform.");
		static tf2_ros::TransformBroadcaster br;
  		geometry_msgs::TransformStamped transformStamped;
  
	    transformStamped.header.stamp = ros::Time::now();
	    transformStamped.header.frame_id = "hummingbird/base_link";
	    transformStamped.child_frame_id = "hummingbird/ortho_base_link";
	    transformStamped.transform.translation.x = 0.0;
	    transformStamped.transform.translation.y = 0.0;
	    transformStamped.transform.translation.z = 0.0;
	    tf2::Quaternion q_ortho;
	    q_ortho.setRPY(-roll, -pitch, 0);
	    transformStamped.transform.rotation.x = q_ortho.x();
	    transformStamped.transform.rotation.y = q_ortho.y();
	    transformStamped.transform.rotation.z = q_ortho.z();
	    transformStamped.transform.rotation.w = q_ortho.w();

	    br.sendTransform(transformStamped);
	}

	void UpdateCarrotOrthoBodyFrame() {
		geometry_msgs::TransformStamped tf;
	    try {

	      tf = tf_buffer_.lookupTransform("hummingbird/ortho_base_link", "world", 
	                                    ros::Time(0), ros::Duration(1.0/30.0));
	    } catch (tf2::TransformException &ex) {
	      ROS_ERROR("%s", ex.what());
	      return;
	    }

	    geometry_msgs::PoseStamped pose_global_goal_world_frame = PoseFromVector3(carrot_world_frame, "world");
	    geometry_msgs::PoseStamped pose_global_goal_ortho_body_frame = PoseFromVector3(Vector3(0,0,0), "hummingbird/ortho_base_link");
	   
	    tf2::doTransform(pose_global_goal_world_frame, pose_global_goal_ortho_body_frame, tf);
	    carrot_ortho_body_frame = VectorFromPose(pose_global_goal_ortho_body_frame);
      //ROS_WARN_STREAM("carrot_ortho_body_frame: " << carrot_ortho_body_frame << " dist: " << carrot_ortho_body_frame.norm());
      if(carrot_ortho_body_frame.norm() < 0.5)
        go_ = false;
	}

	void UpdateAttitudeGeneratorRollPitch(double roll, double pitch) {
		attitude_generator.UpdateRollPitch(roll, pitch);
	}

	void UpdateLaserRDFFramesFromPose() {
		transformAccelerationsIntoLaserRDFFrames();
		TrajectoryLibrary* trajectory_library_ptr = trajectory_selector.GetTrajectoryLibraryPtr();
		Vector3 initial_acceleration = trajectory_library_ptr->getInitialAcceleration();
    	if (trajectory_library_ptr != nullptr) {
			trajectory_library_ptr->setInitialAccelerationLASER(transformOrthoBodyIntoLaserFrame(initial_acceleration));
			trajectory_library_ptr->setInitialAccelerationRDF(transformOrthoBodyIntoRDFFrame(initial_acceleration));
		}
	}

  void OnGT( nav_msgs::Odometry const& odom)
  {
    //ROS_INFO("GOT ODOM");
    OnPose(odom.pose);
    OnVelocity(odom.twist);
  }


	void OnPose( geometry_msgs::PoseWithCovariance const& pose ) {
		//ROS_INFO("GOT POSE");
		attitude_generator.setZ(pose.pose.position.z);
		
		tf::Quaternion q(pose.pose.orientation.x, pose.pose.orientation.y, pose.pose.orientation.z, pose.pose.orientation.w);
		double roll, pitch, yaw;
		tf::Matrix3x3(q).getRPY(roll, pitch, yaw);

		UpdateTrajectoryLibraryRollPitch(roll, pitch);
		UpdateAttitudeGeneratorRollPitch(roll, pitch);
		PublishOrthoBodyTransform(roll, pitch);
		UpdateCarrotOrthoBodyFrame();
		UpdateLaserRDFFramesFromPose();
	}

	void transformAccelerationsIntoLaserRDFFrames() {
		TrajectoryLibrary* trajectory_library_ptr = trajectory_selector.GetTrajectoryLibraryPtr();
		if (trajectory_library_ptr != nullptr) {

			std::vector<Trajectory>::iterator trajectory_iterator_begin = trajectory_library_ptr->GetTrajectoryNonConstIteratorBegin();
	  		std::vector<Trajectory>::iterator trajectory_iterator_end = trajectory_library_ptr->GetTrajectoryNonConstIteratorEnd();

	  		Vector3 acceleration_ortho_body;
			Vector3 acceleration_laser_frame;
			Vector3 acceleration_rdf_frame;


	  		for (auto trajectory = trajectory_iterator_begin; trajectory != trajectory_iterator_end; trajectory++) {
	  			acceleration_ortho_body = trajectory->getAcceleration();

	  			acceleration_laser_frame = transformOrthoBodyIntoLaserFrame(acceleration_ortho_body);
	  			trajectory->setAccelerationLASER(acceleration_laser_frame);

	  			acceleration_rdf_frame = transformOrthoBodyIntoRDFFrame(acceleration_ortho_body);
	  			trajectory->setAccelerationRDF(acceleration_rdf_frame);
	  		} 
	  	}
	}

	Vector3 transformOrthoBodyIntoLaserFrame(Vector3 const& ortho_body_vector) {
		geometry_msgs::TransformStamped tf;
    	try {
     		tf = tf_buffer_.lookupTransform("hummingbird/vi_sensor/camera_depth_optical_center_link", "hummingbird/ortho_base_link", 
                                    ros::Time(0), ros::Duration(1/30.0));
   		} catch (tf2::TransformException &ex) {
     	 	ROS_ERROR("%s", ex.what());
      	return Vector3(0,0,0);
    	}
    	geometry_msgs::PoseStamped pose_ortho_body_vector = PoseFromVector3(ortho_body_vector, "hummingbird/ortho_base_link");
    	geometry_msgs::PoseStamped pose_vector_laser_frame = PoseFromVector3(Vector3(0,0,0), "hummingbird/vi_sensor/camera_depth_optical_center_link");
    	tf2::doTransform(pose_ortho_body_vector, pose_vector_laser_frame, tf);
    	return VectorFromPose(pose_vector_laser_frame);
	}

	Vector3 transformOrthoBodyIntoRDFFrame(Vector3 const& ortho_body_vector) {
		geometry_msgs::TransformStamped tf;
    	try {
     		tf = tf_buffer_.lookupTransform("hummingbird/vi_sensor/camera_depth_optical_center_link", "hummingbird/ortho_base_link", 
                                    ros::Time(0), ros::Duration(1/30.0));
   		} catch (tf2::TransformException &ex) {
     	 	ROS_ERROR("%s", ex.what());
      	return Vector3(0,0,0);
    	}
    	geometry_msgs::PoseStamped pose_ortho_body_vector = PoseFromVector3(ortho_body_vector, "hummingbird/ortho_base_link");
    	geometry_msgs::PoseStamped pose_vector_rdf_frame = PoseFromVector3(Vector3(0,0,0), "hummingbird/vi_sensor/camera_depth_optical_center_link");
    	tf2::doTransform(pose_ortho_body_vector, pose_vector_rdf_frame, tf);
    	return VectorFromPose(pose_vector_rdf_frame);
	}

	Vector3 TransformWorldToOrthoBody(Vector3 const& world_frame) {
		geometry_msgs::TransformStamped tf;
	    try {
	      tf = tf_buffer_.lookupTransform("hummingbird/ortho_base_link", "world", 
	                                    ros::Time(0), ros::Duration(1/30.0));
	    } catch (tf2::TransformException &ex) {
	      ROS_ERROR("%s", ex.what());
	      return Vector3::Zero();
	    }

	    Eigen::Quaternion<Scalar> quat(tf.transform.rotation.w, tf.transform.rotation.x, tf.transform.rotation.y, tf.transform.rotation.z);
	    Matrix3 R = quat.toRotationMatrix();
	    return R*world_frame;
	}

	void UpdateTrajectoryLibraryVelocity(Vector3 const& velocity_ortho_body_frame) {
		TrajectoryLibrary* trajectory_library_ptr = trajectory_selector.GetTrajectoryLibraryPtr();
		if (trajectory_library_ptr != nullptr) {
			trajectory_library_ptr->setInitialVelocity(velocity_ortho_body_frame);
			trajectory_library_ptr->setInitialVelocityLASER(transformOrthoBodyIntoLaserFrame(velocity_ortho_body_frame));
			trajectory_library_ptr->setInitialVelocityRDF(transformOrthoBodyIntoRDFFrame(velocity_ortho_body_frame));
		}
	}

	void OnVelocity( geometry_msgs::TwistWithCovariance const& twist_msg) {
		//ROS_INFO("GOT VELOCITY");
		attitude_generator.setZvelocity(twist_msg.twist.linear.z);
		Vector3 velocity_world_frame(twist_msg.twist.linear.x, twist_msg.twist.linear.y, twist_msg.twist.linear.z);
		Vector3 velocity_ortho_body_frame = TransformWorldToOrthoBody(velocity_world_frame);
		UpdateTrajectoryLibraryVelocity(velocity_ortho_body_frame);
	}
	
	void OnGlobalGoal(geometry_msgs::PoseStamped const& global_goal) {
		//ROS_INFO("GOT GLOBAL GOAL");
		//carrot_world_frame << global_goal.pose.position.x, global_goal.pose.position.y, global_goal.pose.position.z+1.0; 
		carrot_world_frame << global_goal.pose.position.x, global_goal.pose.position.y, current_state.position.z; 
		UpdateCarrotOrthoBodyFrame();

    quad_msgs::QuadDesiredState msg;
    msg.header.stamp = ros::Time::now();
    msg.position = current_state.position;
    msg.velocity = current_state.velocity;
    tf::vectorEigenToMsg(Vector3::Zero(), msg.acceleration);
    tf::vectorEigenToMsg(Vector3::Zero(), msg.jerk);
    desired_state_pub.publish(msg);

    ROS_WARN("[%s] Activating Feedthrough", ros::this_node::getName().c_str());
    std_msgs::Bool feedthrough_msg;
    feedthrough_msg.data = true;
    feedthrough_pub.publish(feedthrough_msg);
    go_ = true;
	}

	void OnScan(sensor_msgs::PointCloud2 const& laser_point_cloud_msg) {
		//ROS_INFO("GOT SCAN");
    last_depth = ros::Time::now();
    
		LaserScanCollisionEvaluator* laser_scan_collision_ptr = trajectory_selector.GetLaserScanCollisionEvaluatorPtr();

		if (laser_scan_collision_ptr != nullptr) {
			
			pcl::PCLPointCloud2* cloud = new pcl::PCLPointCloud2; 
			pcl::PCLPointCloud2ConstPtr cloudPtr(cloud);
			
			pcl_conversions::toPCL(laser_point_cloud_msg, *cloud);
			pcl::PointCloud<pcl::PointXYZ>::Ptr xyz_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>);
			pcl::fromPCLPointCloud2(*cloud,*xyz_cloud);

			laser_scan_collision_ptr->UpdatePointCloudPtr(xyz_cloud);
		}
	}

	void UpdateValueGrid(nav_msgs::OccupancyGrid value_grid_msg) {
		auto t1 = std::chrono::high_resolution_clock::now();

		ValueGridEvaluator* value_grid_evaluator_ptr = trajectory_selector.GetValueGridEvaluatorPtr();
		if (value_grid_evaluator_ptr != nullptr) {
			ValueGrid* value_grid_ptr = value_grid_evaluator_ptr->GetValueGridPtr();
			if (value_grid_ptr != nullptr) {

				value_grid_ptr->SetResolution(value_grid_msg.info.resolution);
				value_grid_ptr->SetSize(value_grid_msg.info.width, value_grid_msg.info.height);
				value_grid_ptr->SetOrigin(value_grid_msg.info.origin.position.x, value_grid_msg.info.origin.position.y);
				value_grid_ptr->SetValues(value_grid_msg.data);

				//trajectory_selector.PassInUpdatedValueGrid(&value_grid);
				auto t2 = std::chrono::high_resolution_clock::now();
				std::cout << "Whole value grid construction took "
		      		<< std::chrono::duration_cast<std::chrono::microseconds>(t2-t1).count()
		      		<< " microseconds\n";

				std::cout << value_grid_ptr->GetValueOfPosition(carrot_world_frame) << " is value of goal" << std::endl;
				std::cout << value_grid_ptr->GetValueOfPosition(Vector3(0,0,0)) << " is value of world origin" << std::endl;
				std::cout << value_grid_ptr->GetValueOfPosition(Vector3(0,-2.0,0)) << " is value of 1.5 to my right" << std::endl;
			}
		}
	}

	void OnValueGrid(nav_msgs::OccupancyGrid value_grid_msg) {
		ROS_INFO("GOT VALUE GRID");
		UpdateValueGrid(value_grid_msg);
	}


	void OnWaypoints(nav_msgs::Path const& waypoints) {
		//ROS_INFO("GOT WAYPOINTS");
		int waypoints_to_check = std::min((int) waypoints.poses.size(), max_waypoints);
		nh.param("carrot_distance", carrot_distance, 0.5);

		waypoints_matrix.resize(4, waypoints_to_check);
		waypoints_matrix.col(0) << VectorFromPose(waypoints.poses[0]), 0.0;  // yaw is currently hard set to be 0
		double distance_so_far = 0.0;
		double distance_to_add;
		double distance_left;
		Eigen::Vector3d truncated_waypoint;
		Eigen::Vector3d p1, p2;
		int i;
		for (i = 0; i < waypoints_to_check - 1; i++){
			p1 = VectorFromPose(waypoints.poses[i]);
			p2 = VectorFromPose(waypoints.poses[i+1]);
			distance_to_add = (p2-p1).norm();
			if ((distance_to_add + distance_so_far) < carrot_distance) {
				distance_so_far += distance_to_add;
				waypoints_matrix.col(i + 1) << p2, 0.0; // yaw is currently hard set to be 0
			}
			else {
				distance_left = carrot_distance - distance_so_far;
				truncated_waypoint = p1 + (p2-p1) / distance_to_add * distance_left;
				distance_so_far = distance_so_far + distance_left;
				waypoints_matrix.col(i + 1) << truncated_waypoint, 0.0; // yaw is currently hard set to be 0
				i++;
				break;

			}
		}
		carrot_world_frame << waypoints_matrix(0, i), waypoints_matrix(1, i), waypoints_matrix(2, i); 
		//attitude_generator.setZsetpoint(carrot_world_frame(2));
		


		geometry_msgs::TransformStamped tf;
	    try {
	      // Need to remove leading "/" if it exists.
	      std::string pose_frame_id = waypoints.poses[0].header.frame_id;
	      if (pose_frame_id[0] == '/') {
	        pose_frame_id = pose_frame_id.substr(1, pose_frame_id.size()-1);
	      }

	      tf = tf_buffer_.lookupTransform("hummingbird/ortho_base_link", pose_frame_id, 
	                                    ros::Time(0), ros::Duration(1/30.0));
	    } catch (tf2::TransformException &ex) {
	      ROS_ERROR("%s", ex.what());
	      return;
	    }

	    geometry_msgs::PoseStamped pose_carrot_world_frame = PoseFromVector3(carrot_world_frame, "world");
	    geometry_msgs::PoseStamped pose_carrot_ortho_body_frame = PoseFromVector3(carrot_ortho_body_frame, "hummingbird/ortho_base_link");
	   
	    tf2::doTransform(pose_carrot_world_frame, pose_carrot_ortho_body_frame, tf);

	    carrot_ortho_body_frame = VectorFromPose(pose_carrot_ortho_body_frame);


	    visualization_msgs::Marker marker;
		marker.header.frame_id = "hummingbird/ortho_base_link";
		marker.header.stamp = ros::Time::now();
		marker.ns = "my_namespace";
		marker.id = 0;
		marker.type = visualization_msgs::Marker::SPHERE;
		marker.action = visualization_msgs::Marker::ADD;
		marker.pose.position.x = carrot_ortho_body_frame(0);
		marker.pose.position.y = carrot_ortho_body_frame(1);
		marker.pose.position.z = carrot_ortho_body_frame(2);
		marker.scale.x = 0.5;
		marker.scale.y = 0.5;
		marker.scale.z = 0.5;
		marker.color.a = 0.5; // Don't forget to set the alpha!
		marker.color.r = 0.9;
		marker.color.g = 0.4;
		marker.color.b = 0.0;
		carrot_pub.publish( marker );

	}


  void CopilotFeedbackCallback(const quad_msgs::ControllerFeedbackConstPtr& msg)
  {
    current_state = msg->state_estimate;
    if(msg->controller_state == 8) // feedthrough
    {
      copilot_is_in_feedthrough = true;
    }
  }


	void OnDepthImage(const sensor_msgs::PointCloud2ConstPtr& point_cloud_msg) {
		ROS_INFO("GOT POINT CLOUD");
		DepthImageCollisionEvaluator* depth_image_collision_ptr = trajectory_selector.GetDepthImageCollisionEvaluatorPtr();

		if (depth_image_collision_ptr != nullptr) {


			pcl::PCLPointCloud2* cloud = new pcl::PCLPointCloud2; 
			pcl::PCLPointCloud2ConstPtr cloudPtr(cloud);
			
	    	pcl_conversions::toPCL(*point_cloud_msg, *cloud);
	    	pcl::PointCloud<pcl::PointXYZ>::Ptr xyz_cloud(new pcl::PointCloud<pcl::PointXYZ>);
	    	pcl::fromPCLPointCloud2(*cloud,*xyz_cloud);

			depth_image_collision_ptr->UpdatePointCloudPtr(xyz_cloud);
		}
	
	}

  void PublishDesiredState(Vector3 const& desired_acceleration, 
                           Vector3 const& desired_velocity, 
                           Vector3 const& desired_position, 
                           Vector3 const& desired_jerk)
  {
    quad_msgs::QuadDesiredState msg;
    msg.header.stamp = ros::Time::now();
    tf::vectorEigenToMsg(desired_position, msg.position);
    tf::vectorEigenToMsg(desired_velocity, msg.velocity);
    tf::vectorEigenToMsg(desired_acceleration, msg.acceleration);
    tf::vectorEigenToMsg(desired_jerk, msg.jerk);
    desired_state_pub.publish(msg);
  }
	
	void PublishAttitudeSetpoint(Vector3 const& roll_pitch_thrust) { 

    /*
		using namespace Eigen;

		// Vector3 pid;
		// double offset;
		// nh.param("z_p", pid(0), 1.5);
		// nh.param("z_i", pid(1), 0.6);
		// nh.param("z_d", pid(2), 0.5);
		// nh.param("z_offset", offset, 0.69);
		// attitude_generator.setGains(pid, offset);

		mavros_msgs::AttitudeTarget setpoint_msg;
		setpoint_msg.header.stamp = ros::Time::now();
		setpoint_msg.type_mask = mavros_msgs::AttitudeTarget::IGNORE_ROLL_RATE 
			| mavros_msgs::AttitudeTarget::IGNORE_PITCH_RATE
			| mavros_msgs::AttitudeTarget::IGNORE_YAW_RATE
			;

		double bearing_azimuth_degrees = 0;
		
		// uncomment below for bearing control
		//nh.param("bearing_azimuth_degrees", bearing_azimuth_degrees, 0.0);

		Matrix3f m;
		m =AngleAxisf(-bearing_azimuth_degrees*M_PI/180.0, Vector3f::UnitZ())
		* AngleAxisf(roll_pitch_thrust(1), Vector3f::UnitY())
		* AngleAxisf(-roll_pitch_thrust(0), Vector3f::UnitX());

		Quaternionf q(m);

		setpoint_msg.orientation.w = q.w();
		setpoint_msg.orientation.x = q.x();
		setpoint_msg.orientation.y = q.y();
		setpoint_msg.orientation.z = q.z();

		setpoint_msg.thrust = roll_pitch_thrust(2);

		attitude_thrust_pub.publish(setpoint_msg);
    */

    quad_msgs::AttitudeYawRateCommand setpoint_msg;
		setpoint_msg.header.stamp = ros::Time::now();
		setpoint_msg.execution_time = ros::Time::now();
		setpoint_msg.roll = roll_pitch_thrust[0];
		setpoint_msg.pitch = roll_pitch_thrust[1];
    setpoint_msg.yawrate = 0.0;
		setpoint_msg.thrust = roll_pitch_thrust[2];
    attitude_thrust_pub.publish(setpoint_msg);

		// To visualize setpoint

		// geometry_msgs::PoseStamped attitude_setpoint;
		// attitude_setpoint.header.frame_id = "world";
		// attitude_setpoint.header.stamp = ros::Time::now();
		// Vector3 initial_acceleration = trajectory_selector.getInitialAcceleration();
		// attitude_setpoint.pose.position.x = initial_acceleration(0);
		// attitude_setpoint.pose.position.y = initial_acceleration(1);
		// attitude_setpoint.pose.position.z = initial_acceleration(2)+5;
		// attitude_setpoint.pose.orientation = setpoint_msg.orientation;
		// attitude_setpoint_visualization_pub.publish( attitude_setpoint );

	}


	ros::Subscriber waypoints_sub;
	ros::Subscriber gt_sub;
	ros::Subscriber pose_sub;
	ros::Subscriber velocity_sub;
	ros::Subscriber depth_image_sub;
	ros::Subscriber global_goal_sub;
	ros::Subscriber value_grid_sub;
	ros::Subscriber laser_scan_sub;
	ros::Subscriber copilot_feedback_sub;

	ros::Publisher carrot_pub;
	ros::Publisher gaussian_pub;
	ros::Publisher attitude_thrust_pub;
	ros::Publisher desired_state_pub;
	ros::Publisher attitude_setpoint_visualization_pub;
  ros::Publisher feedthrough_pub;

	std::vector<ros::Publisher> action_paths_pubs;

	std::shared_ptr<tf2_ros::TransformListener> tf_listener_;
	tf2_ros::Buffer tf_buffer_;

	nav_msgs::Path waypoints;
	nav_msgs::Path previous_waypoints;
	int max_waypoints = 6;
	double carrot_distance;

	double start_time = 0.0;
	double final_time = 1.0;

	Eigen::Vector4d pose_x_y_z_yaw;
	Eigen::Matrix<double, 4, Eigen::Dynamic> waypoints_matrix;

	Eigen::Matrix<Scalar, Eigen::Dynamic, 1> sampling_time_vector;
	size_t num_samples;

	std::mutex mutex;

	Vector3 carrot_world_frame;
	Vector3 carrot_ortho_body_frame;

	size_t best_traj_index = 0;

	TrajectorySelector trajectory_selector;
	AttitudeGenerator attitude_generator;

  bool go_;
  bool copilot_is_in_feedthrough;
  ros::Time last_depth;
  quad_msgs::QuadStateEstimate current_state;

	ros::NodeHandle nh;

public:
	TrajectoryVisualizer trajectory_visualizer;
	EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};


int main(int argc, char* argv[]) {
	std::cout << "Initializing trajectory_selector_node" << std::endl;

	ros::init(argc, argv, "TrajectorySelectorNode");

	TrajectorySelectorNode trajectory_selector_node;

	std::cout << "Got through to here" << std::endl;
	ros::Rate spin_rate(50);

	auto t1 = std::chrono::high_resolution_clock::now();
	auto t2 = std::chrono::high_resolution_clock::now();

	while (ros::ok()) {
		//t1 = std::chrono::high_resolution_clock::now();
		trajectory_selector_node.ReactToSampledPointCloud();
		//t2 = std::chrono::high_resolution_clock::now();
		// std::cout << "ReactToSampledPointCloud took "
  //     		<< std::chrono::duration_cast<std::chrono::microseconds>(t2-t1).count()
  //     		<< " microseconds\n";
      	

		trajectory_selector_node.trajectory_visualizer.drawAll();

		ros::spinOnce();
		spin_rate.sleep();
	}
}
