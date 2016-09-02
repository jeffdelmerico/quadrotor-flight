#include "trajectory_selector.h"

TrajectoryLibrary* TrajectorySelector::GetTrajectoryLibraryPtr() {
  return &trajectory_library;
};

ValueGridEvaluator* TrajectorySelector::GetValueGridEvaluatorPtr() {
  return &value_grid_evaluator;
};

LaserScanCollisionEvaluator* TrajectorySelector::GetLaserScanCollisionEvaluatorPtr() {
  return &laser_scan_collision_evaluator;
};

DepthImageCollisionEvaluator* TrajectorySelector::GetDepthImageCollisionEvaluatorPtr() {
  return &depth_image_collision_evaluator;
};


void TrajectorySelector::InitializeLibrary(double const& final_time) {
  trajectory_library.Initialize2DLibrary(final_time);
  this->final_time = final_time;

  size_t num_samples = 10;
  double sampling_time = 0;
  double sampling_interval = (final_time - start_time) / num_samples;
  for (size_t sample_index = 0; sample_index < num_samples; sample_index++) {
      sampling_time = start_time + sampling_interval*(sample_index+1);
      sampling_time_vector(sample_index) = sampling_time;
  }

  sampling_time = 0;
  sampling_interval = (final_time - start_time) / num_samples_collision;
  for (size_t sample_index = 0; sample_index < num_samples_collision; sample_index++) {
      sampling_time = start_time + sampling_interval*(sample_index+1);
      collision_sampling_time_vector(sample_index) = sampling_time;
  }
};


size_t TrajectorySelector::getNumTrajectories() {
  return trajectory_library.getNumTrajectories();
};


// Euclidean Evaluator
void TrajectorySelector::computeBestEuclideanTrajectory(Vector3 const& carrot_body_frame, 
                                                        size_t &best_traj_index, 
                                                        Vector3& desired_acceleration,
                                                        Vector3& desired_velocity,
                                                        Vector3& desired_position,
                                                        Vector3& desired_jerk,
                                                        Scalar const& t) 
{
  EvaluateCollisionProbabilities();
  EvaluateGoalProgress(carrot_body_frame); 
  EvaluateTerminalVelocityCost();
  EvaluateObjectivesEuclid();

  desired_acceleration << 0,0,0;
  best_traj_index = 0;
  float current_objective_value;
  float best_traj_objective_value = objectives_euclid(0);
  for (size_t traj_index = 1; traj_index < 25; traj_index++) {
    current_objective_value = objectives_euclid(traj_index);
    if (current_objective_value > best_traj_objective_value) {
      best_traj_index = traj_index;
      best_traj_objective_value = current_objective_value;
    }
  }
  Trajectory traj = trajectory_library.getTrajectoryFromIndex(best_traj_index);
  desired_acceleration = traj.getAcceleration();
  desired_velocity = traj.getVelocity(t);
  desired_position = traj.getPosition(t);
  desired_jerk = traj.getJerk(t);
};

void TrajectorySelector::EvaluateObjectivesEuclid() {
  for (int i = 0; i < 25; i++) {
    objectives_euclid(i) = EvaluateWeightedObjectiveEuclid(i);
  }
  objectives_euclid = MakeAllGreaterThan1(objectives_euclid);
  no_collision_probabilities = Normalize0to1(no_collision_probabilities);
  objectives_euclid = objectives_euclid.cwiseProduct(no_collision_probabilities);
}

double TrajectorySelector::EvaluateWeightedObjectiveEuclid(size_t const& trajectory_index) {
  return goal_progress_evaluations(trajectory_index) + 1.0*terminal_velocity_evaluations(trajectory_index);
}


// Dijkstra Evaluator
void TrajectorySelector::computeBestDijkstraTrajectory(Vector3 const& carrot_body_frame, Vector3 const& carrot_world_frame, geometry_msgs::TransformStamped const& tf, size_t &best_traj_index, Vector3 &desired_acceleration) {
  EvaluateCollisionProbabilities();
  EvaluateDijkstraCost(carrot_world_frame, tf);
  EvaluateGoalProgress(carrot_body_frame); 
  EvaluateTerminalVelocityCost();
  EvaluateObjectivesDijkstra();

  desired_acceleration << 0,0,0;
  best_traj_index = 0;
  double current_objective_value;
  double best_traj_objective_value = objectives_dijkstra(0);
  for (size_t traj_index = 1; traj_index < 25; traj_index++) {
    current_objective_value = objectives_dijkstra(traj_index);
    if (current_objective_value > best_traj_objective_value) {
      best_traj_index = traj_index;
      best_traj_objective_value = current_objective_value;
    }
  }
  desired_acceleration = trajectory_library.getTrajectoryFromIndex(best_traj_index).getAcceleration();
  return;
}

void TrajectorySelector::EvaluateObjectivesDijkstra() {
  for (int i = 0; i < 25; i++) {
    objectives_dijkstra(i) = EvaluateWeightedObjectiveDijkstra(i);
  }
  objectives_dijkstra = MakeAllGreaterThan1(objectives_dijkstra);
  no_collision_probabilities = Normalize0to1(no_collision_probabilities);
  objectives_dijkstra = objectives_dijkstra.cwiseProduct(no_collision_probabilities);
}




double TrajectorySelector::EvaluateWeightedObjectiveDijkstra(size_t index) {
  return dijkstra_evaluations(index) + 0.2*goal_progress_evaluations(index) + 1.0*terminal_velocity_evaluations(index);
}



void TrajectorySelector::EvaluateDijkstraCost(Vector3 const& carrot_world_frame, geometry_msgs::TransformStamped const& tf) {

  ValueGrid* value_grid_ptr = value_grid_evaluator.GetValueGridPtr();

  std::vector<Trajectory>::const_iterator trajectory_iterator_begin = trajectory_library.GetTrajectoryIteratorBegin();
  std::vector<Trajectory>::const_iterator trajectory_iterator_end = trajectory_library.GetTrajectoryIteratorEnd();

  size_t i = 0;
  Vector3 ortho_body_frame_position;
  geometry_msgs::PoseStamped pose_ortho_body_frame_position;
  geometry_msgs::PoseStamped pose_world_frame_position = PoseFromVector3(Vector3(0,0,0), "world");
  int current_value;
  // Iterate over trajectories
  for (auto trajectory = trajectory_iterator_begin; trajectory != trajectory_iterator_end; trajectory++) {
    
   dijkstra_evaluations(i) = 0;
    double sampling_time;
    //Iterate over sampling times
    for (size_t time_index = 0; time_index < sampling_time_vector.size(); time_index++) {
      
      sampling_time = sampling_time_vector(time_index);
      ortho_body_frame_position = trajectory->getPosition(sampling_time);
      
      geometry_msgs::PoseStamped pose_ortho_body_frame_position = PoseFromVector3(ortho_body_frame_position, "ortho_body");
      tf2::doTransform(pose_ortho_body_frame_position, pose_world_frame_position, tf);
      Vector3 world_frame_position = VectorFromPose(pose_world_frame_position);

      // // Then evaluate Dijkstra cost
      current_value = value_grid_ptr->GetValueOfPosition(world_frame_position);

      if ((current_value == 0) && ((world_frame_position - carrot_world_frame).norm() > 1.0)) {
        current_value = 1000;
      }

      dijkstra_evaluations(i) -= current_value;

    }
    
    i++;
  }
  //std::cout << "At the end of all this, my Dijkstra evaluations are: " << dijkstra_evaluations << std::endl;
};








void TrajectorySelector::EvaluateGoalProgress(Vector3 const& carrot_body_frame) {

  // for each traj in trajectory_library.trajectories
  std::vector<Trajectory>::const_iterator trajectory_iterator_begin = trajectory_library.GetTrajectoryIteratorBegin();
  std::vector<Trajectory>::const_iterator trajectory_iterator_end = trajectory_library.GetTrajectoryIteratorEnd();

  double initial_distance = carrot_body_frame.norm();

  //std::cout << "initial_distance is " << initial_distance << std::endl;

  size_t i = 0;
  Vector3 final_trajectory_position;
  double distance;
  for (auto trajectory = trajectory_iterator_begin; trajectory != trajectory_iterator_end; trajectory++) {
    final_trajectory_position = trajectory->getTerminalStopPosition(final_time);
    // if (final_trajectory_position.norm() < initial_distance) {
    //   final_trajectory_position = trajectory->getPosition(final_time);
    // }
    distance = (final_trajectory_position - carrot_body_frame).norm();
    goal_progress_evaluations(i) = initial_distance - distance; 
    i++;
  }
};


void TrajectorySelector::EvaluateTerminalVelocityCost() {

  // for each traj in trajectory_library.trajectories
  std::vector<Trajectory>::const_iterator trajectory_iterator_begin = trajectory_library.GetTrajectoryIteratorBegin();
  std::vector<Trajectory>::const_iterator trajectory_iterator_end = trajectory_library.GetTrajectoryIteratorEnd();

  size_t i = 0;
  double final_trajectory_speed;
  double distance;
  for (auto trajectory = trajectory_iterator_begin; trajectory != trajectory_iterator_end; trajectory++) {
    final_trajectory_speed = trajectory->getVelocity(0.5).norm();
    terminal_velocity_evaluations(i) = 0;
    
    // cost on going too fast
    if (final_trajectory_speed > (soft_top_speed-1.0)) {
      terminal_velocity_evaluations(i) -= ((soft_top_speed-1.0) - final_trajectory_speed)*((soft_top_speed-1.0) - final_trajectory_speed);
    }

    i++;
  }
};



void TrajectorySelector::EvaluateCollisionProbabilities() {
  
  // for each traj in trajectory_library.trajectories
  std::vector<Trajectory>::const_iterator trajectory_iterator_begin = trajectory_library.GetTrajectoryIteratorBegin();
  std::vector<Trajectory>::const_iterator trajectory_iterator_end = trajectory_library.GetTrajectoryIteratorEnd();

  size_t i = 0;
  for (auto trajectory = trajectory_iterator_begin; trajectory != trajectory_iterator_end; trajectory++) {
    collision_probabilities(i) = computeProbabilityOfCollisionOneTrajectory(*trajectory);   
    i++;
  }
  for (int i = 0; i < 25; i++) {
    no_collision_probabilities(i) = 1.0 - collision_probabilities(i);
  }
};

double TrajectorySelector::computeProbabilityOfCollisionOneTrajectory(Trajectory trajectory) {
  double probability_no_collision = 1;
  double probability_of_collision_one_step = 0.0;
  double probability_no_collision_one_step = 1.0;
  Vector3 robot_position;
  Vector3 sigma_robot_position;

  for (size_t time_step_index = 0; time_step_index < num_samples_collision; time_step_index++) {
    //sigma_robot_position = trajectory_library.getLASERSigmaAtTime(collision_sampling_time_vector(time_step_index)); 
    
    sigma_robot_position = Vector3(0.01,0.01,0.01);
    robot_position = trajectory.getPositionRDF(collision_sampling_time_vector(time_step_index));
    
    probability_of_collision_one_step = depth_image_collision_evaluator.computeProbabilityOfCollisionOnePositionBlock(robot_position, sigma_robot_position, 10);
    //probability_of_collision_one_step = depth_image_collision_evaluator.computeProbabilityOfCollisionOnePositionBlockMarching(robot_position, sigma_robot_position, 50);
    // if (depth_image_collision_evaluator.computeDeterministicCollisionOnePositionKDTree(robot_position, sigma_robot_position)) {
    //   return 1.0;
    // }

    //probability_of_collision_one_step = laser_scan_collision_evaluator.computeProbabilityOfCollisionOnePosition(robot_position, sigma_robot_position);
    probability_no_collision_one_step = 1.0 - probability_of_collision_one_step;
    probability_no_collision = probability_no_collision * probability_no_collision_one_step;
    
  }
  if (probability_no_collision > 1.0) { probability_no_collision = 1.0;};
  if (probability_no_collision < 0.0) { probability_no_collision = 0.0;};
  return 1 - probability_no_collision;

};

Eigen::Matrix<Scalar, 25, 1> TrajectorySelector::Normalize0to1(Eigen::Matrix<Scalar, 25, 1> cost) {
  double max = cost(0);
  double min = cost(0);
  double current;
  for (int i = 1; i < 25; i++) {
    current = cost(i);
    if (current > max) {
      max = current;
    }
    if (current < min) {
      min = current;
    }
  }

  if (max ==min) {
    return cost;
  }

  for (int i = 0; i < 25; i++) {
     cost(i) =  (cost(i)-min)/(max-min);
  }
  return cost;
}

Eigen::Matrix<Scalar, 25, 1> TrajectorySelector::MakeAllGreaterThan1(Eigen::Matrix<Scalar, 25, 1> cost) {
  double min = cost(0);
  double current;
  for (int i = 1; i < 25; i++) {
    current = cost(i);
    if (current < min) {
      min = current;
    }
  }

  for (int i = 0; i < 25; i++) {
     cost(i) =  cost(i)-min+1.0;
  }
  return cost;
}

Eigen::Matrix<Scalar, 25, 1> TrajectorySelector::FilterSmallProbabilities(Eigen::Matrix<Scalar, 25, 1> to_filter) {
  for (size_t i = 0; i < 25; i++) {
    if (to_filter(i) < 0.10) {
      to_filter(i) = 0.0;
    }
  }
  return to_filter;
}

Eigen::Matrix<Scalar, Eigen::Dynamic, 3> TrajectorySelector::sampleTrajectoryForDrawing(size_t trajectory_index, Eigen::Matrix<Scalar, Eigen::Dynamic, 1> sampling_time_vector, size_t num_samples) {
  Trajectory trajectory_to_sample = trajectory_library.getTrajectoryFromIndex(trajectory_index);
  double sampling_time;
  Eigen::Matrix<Scalar, Eigen::Dynamic, 3> sample_points_xyz_over_time(num_samples,3);

  for (size_t time_index = 0; time_index < num_samples; time_index++) {
    //std::cout << "the position I sample is " << trajectory_to_sample.getPosition(sampling_time) << std::endl;
    sampling_time = sampling_time_vector(time_index);
    if (time_index < num_samples - 1) {
      sample_points_xyz_over_time.row(time_index) = trajectory_to_sample.getPositionRDF(sampling_time);
    }
    else {
      sample_points_xyz_over_time.row(time_index) = trajectory_to_sample.getPositionRDF(sampling_time);
    }
  }
  return sample_points_xyz_over_time;
}
