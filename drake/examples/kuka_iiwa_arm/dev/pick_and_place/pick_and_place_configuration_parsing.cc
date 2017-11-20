#include "drake/examples/kuka_iiwa_arm/dev/pick_and_place/pick_and_place_configuration_parsing.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "google/protobuf/text_format.h"

#include "drake/common/drake_throw.h"
#include "drake/common/eigen_types.h"
#include "drake/common/find_resource.h"
#include "drake/common/proto/protobuf.h"
#include "drake/examples/kuka_iiwa_arm/dev/pick_and_place/pick_and_place_configuration.pb.h"
#include "drake/manipulation/util/world_sim_tree_builder.h"
#include "drake/math/roll_pitch_yaw.h"

namespace drake {
namespace examples {
namespace kuka_iiwa_arm {
namespace pick_and_place {

using manipulation::util::WorldSimTreeBuilder;
using math::rpy2rotmat;
using pick_and_place::RobotBaseIndex;
using pick_and_place::TargetIndex;

namespace {
proto::PickAndPlaceConfiguration ReadProtobufFileOrThrow(
    const std::string& filename) {
  auto istream =
      drake::MakeFileInputStreamOrThrow(FindResourceOrThrow(filename));
  proto::PickAndPlaceConfiguration configuration;
  google::protobuf::TextFormat::Parse(istream.get(), &configuration);
  return configuration;
}

Isometry3<double> ParsePose(const proto::Pose& pose) {
  Isometry3<double> X{Isometry3<double>::Identity()};
  if (!pose.xyz().empty()) {
    DRAKE_THROW_UNLESS(pose.xyz_size() == 3);
    X.translation() = Vector3<double>(pose.xyz(0), pose.xyz(1), pose.xyz(2));
  }
  if (!pose.rpy().empty()) {
    DRAKE_THROW_UNLESS(pose.rpy_size() == 3);
    X.linear() =
        rpy2rotmat(Vector3<double>(pose.rpy(0), pose.rpy(1), pose.rpy(2)));
  }
  return X;
}

const proto::Model& GetModelOrThrow(
    const proto::PickAndPlaceConfiguration& configuration,
    const proto::ModelInstance& model_instance) {
  return configuration.model().at(model_instance.name());
}

const std::string& GetSimulationModelPathOrThrow(
    const proto::PickAndPlaceConfiguration& configuration,
    const proto::ModelInstance& model_instance) {
  return GetModelOrThrow(configuration, model_instance).simulation_model_path();
}

const std::string& GetPlanningModelPathOrThrow(
    const proto::PickAndPlaceConfiguration& configuration,
    const proto::ModelInstance& model_instance) {
  const proto::Model& model = GetModelOrThrow(configuration, model_instance);
  return (model.planning_model_path().empty()) ? model.simulation_model_path()
                                               : model.planning_model_path();
}

void ExtractBasePosesForModels(
    const google::protobuf::RepeatedPtrField<proto::ModelInstance>& models,
    std::vector<Isometry3<double>>* poses) {
  DRAKE_DEMAND(poses != nullptr);
  std::transform(models.cbegin(), models.cend(), std::back_inserter(*poses),
                 [](const proto::ModelInstance& model) -> Isometry3<double> {
                   return ParsePose(model.pose());
                 });
}

void ExtractOptitrackInfoForModels(
    const google::protobuf::RepeatedPtrField<proto::ModelInstance>& models,
    std::vector<pick_and_place::OptitrackInfo>* optitrack_info) {
  DRAKE_DEMAND(optitrack_info != nullptr);
  std::transform(
      models.cbegin(), models.cend(), std::back_inserter(*optitrack_info),
      [](const proto::ModelInstance& model) -> pick_and_place::OptitrackInfo {
        return pick_and_place::OptitrackInfo(
            {model.optitrack_info().id(),
             ParsePose(model.optitrack_info().x_mf())});
      });
}

void ExtractModelPathsForModels(
    const proto::PickAndPlaceConfiguration& configuration,
    const google::protobuf::RepeatedPtrField<proto::ModelInstance>& models,
    std::vector<std::string>* model_paths) {
  DRAKE_DEMAND(model_paths != nullptr);
  std::transform(
      models.cbegin(), models.cend(), std::back_inserter(*model_paths),
      [&configuration](const proto::ModelInstance& table) -> std::string {
        return GetSimulationModelPathOrThrow(configuration, table);
      });
}

pick_and_place::PlannerConfiguration DoParsePlannerConfiguration(
    const proto::PickAndPlaceConfiguration& configuration,
    const std::string& end_effector_name,
    pick_and_place::RobotBaseIndex robot_index,
    pick_and_place::TargetIndex target_index) {
  // Check that the robot base and target indices are valid.
  DRAKE_THROW_UNLESS(robot_index < configuration.robot_size());
  DRAKE_THROW_UNLESS(target_index < configuration.object_size());

  pick_and_place::PlannerConfiguration planner_configuration;

  // Set the robot and target indices
  planner_configuration.robot_index = robot_index;
  planner_configuration.target_index = target_index;

  // Store model path and end-effector name.
  planner_configuration.model_path = GetPlanningModelPathOrThrow(
      configuration, configuration.robot(planner_configuration.robot_index));
  planner_configuration.end_effector_name = end_effector_name;

  // Extract number of tables
  planner_configuration.num_tables = configuration.table_size();

  // Extract target dimensions
  WorldSimTreeBuilder<double> tree_builder;
  tree_builder.StoreModel(
      "target", GetPlanningModelPathOrThrow(
                    configuration,
                    configuration.object(planner_configuration.target_index)));
  tree_builder.AddFixedModelInstance("target", Vector3<double>::Zero());
  auto target = tree_builder.Build();
  Vector3<double> min_corner{
      Vector3<double>::Constant(std::numeric_limits<double>::infinity())};
  Vector3<double> max_corner{-min_corner};
  // The target dimensions for planning will be the side lengths of the
  // axis-aligned bounding box for all VISUAL geometry of the target. Ideally,
  // this would be the collision geometry, but RigidBody doesn't give const
  // access to collision geometries.
  for (const auto& visual_element :
       target->get_body(target->FindBaseBodies()[0]).get_visual_elements()) {
    Matrix3X<double> bounding_box_points;
    visual_element.getGeometry().getBoundingBoxPoints(bounding_box_points);
    const Isometry3<double> X_BV = visual_element.getLocalTransform();
    max_corner =
        max_corner.cwiseMax((X_BV * bounding_box_points).rowwise().maxCoeff());
    min_corner =
        min_corner.cwiseMin((X_BV * bounding_box_points).rowwise().minCoeff());
  }
  planner_configuration.target_dimensions = max_corner - min_corner;

  return planner_configuration;
}
}  // namespace

pick_and_place::PlannerConfiguration ParsePlannerConfigurationOrThrow(
    const std::string& filename, TaskIndex task_index) {
  // Read configuration file
  const proto::PickAndPlaceConfiguration configuration{
      ReadProtobufFileOrThrow(filename)};

  // Check that the task index is valid.
  DRAKE_THROW_UNLESS(task_index < configuration.task_size());
  RobotBaseIndex robot_index(configuration.task(task_index).robot_index());
  TargetIndex target_index(configuration.task(task_index).target_index());

  return DoParsePlannerConfiguration(
      configuration, configuration.task(task_index).end_effector_name(),
      robot_index, target_index);
}

pick_and_place::PlannerConfiguration ParsePlannerConfigurationOrThrow(
    const std::string& filename, const std::string& end_effector_name,
    RobotBaseIndex robot_index, TargetIndex target_index) {
  // Read configuration file
  const proto::PickAndPlaceConfiguration configuration{
      ReadProtobufFileOrThrow(filename)};

  // Check that the robot and target indices are valid.
  DRAKE_THROW_UNLESS(robot_index < configuration.robot_size());
  DRAKE_THROW_UNLESS(target_index < configuration.object_size());

  return DoParsePlannerConfiguration(configuration, end_effector_name,
                                     robot_index, target_index);
}

std::vector<pick_and_place::PlannerConfiguration>
ParsePlannerConfigurationsOrThrow(const std::string& filename) {
  // Read configuration file
  const proto::PickAndPlaceConfiguration configuration{
      ReadProtobufFileOrThrow(filename)};

  // Build the planner configuration vector.
  std::vector<pick_and_place::PlannerConfiguration> planner_configurations;
  std::transform(configuration.task().cbegin(), configuration.task().cend(),
                 std::back_inserter(planner_configurations),
                 [&configuration](const proto::PickAndPlaceTask& task)
                     -> pick_and_place::PlannerConfiguration {
                   return DoParsePlannerConfiguration(
                       configuration, task.end_effector_name(),
                       pick_and_place::RobotBaseIndex(task.robot_index()),
                       pick_and_place::TargetIndex(task.target_index()));
                 });
  return planner_configurations;
}

pick_and_place::SimulatedPlantConfiguration
ParseSimulatedPlantConfigurationOrThrow(const std::string& filename) {
  pick_and_place::SimulatedPlantConfiguration plant_configuration;

  // Read configuration file
  const proto::PickAndPlaceConfiguration configuration{
      ReadProtobufFileOrThrow(filename)};

  // Extract robot model paths
  ExtractModelPathsForModels(configuration, configuration.robot(),
                             &plant_configuration.robot_models);

  // Extract base positions
  ExtractBasePosesForModels(configuration.robot(),
                            &plant_configuration.robot_poses);

  // Extract table model paths
  ExtractModelPathsForModels(configuration, configuration.table(),
                             &plant_configuration.table_models);

  // Extract table poses
  ExtractBasePosesForModels(configuration.table(),
                            &plant_configuration.table_poses);

  // Extract object model paths
  ExtractModelPathsForModels(configuration, configuration.object(),
                             &plant_configuration.object_models);

  // Extract object poses
  ExtractBasePosesForModels(configuration.object(),
                            &plant_configuration.object_poses);

  if (configuration.static_friction_coefficient() > 0) {
    plant_configuration.static_friction_coef =
        configuration.static_friction_coefficient();
  }
  if (configuration.dynamic_friction_coefficient() > 0) {
    plant_configuration.dynamic_friction_coef =
        configuration.dynamic_friction_coefficient();
  }
  if (configuration.v_stiction_tolerance() > 0) {
    plant_configuration.v_stiction_tolerance =
        configuration.v_stiction_tolerance();
  }
  if (configuration.stiffness() > 0) {
    plant_configuration.stiffness = configuration.stiffness();
  }
  if (configuration.dissipation() > 0) {
    plant_configuration.dissipation = configuration.dissipation();
  }

  return plant_configuration;
}

pick_and_place::OptitrackConfiguration ParseOptitrackConfigurationOrThrow(
    const std::string& filename) {
  pick_and_place::OptitrackConfiguration optitrack_configuration;

  // Read configuration file
  const proto::PickAndPlaceConfiguration configuration{
      ReadProtobufFileOrThrow(filename)};

  // Extract Optitrack info for tables
  ExtractOptitrackInfoForModels(configuration.table(),
                                &optitrack_configuration.table_optitrack_info);

  // Extract Optitrack Ids for objects
  ExtractOptitrackInfoForModels(configuration.object(),
                                &optitrack_configuration.object_optitrack_info);

  // Extract Optitrack Ids for robot bases
  ExtractOptitrackInfoForModels(
      configuration.robot(),
      &optitrack_configuration.robot_base_optitrack_info);

  return optitrack_configuration;
}

}  // namespace pick_and_place
}  // namespace kuka_iiwa_arm
}  // namespace examples
}  // namespace drake