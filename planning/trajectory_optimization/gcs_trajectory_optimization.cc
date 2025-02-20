#include "drake/planning/trajectory_optimization/gcs_trajectory_optimization.h"

#include <limits>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "drake/common/pointer_cast.h"
#include "drake/common/scope_exit.h"
#include "drake/common/symbolic/decompose.h"
#include "drake/geometry/optimization/cartesian_product.h"
#include "drake/geometry/optimization/hpolyhedron.h"
#include "drake/geometry/optimization/point.h"
#include "drake/math/matrix_util.h"
#include "drake/solvers/solve.h"

namespace drake {
namespace planning {
namespace trajectory_optimization {

using Subgraph = GcsTrajectoryOptimization::Subgraph;
using EdgesBetweenSubgraphs = GcsTrajectoryOptimization::EdgesBetweenSubgraphs;

using Eigen::MatrixXd;
using Eigen::VectorXd;
using geometry::optimization::CartesianProduct;
using geometry::optimization::ConvexSet;
using geometry::optimization::ConvexSets;
using geometry::optimization::GraphOfConvexSets;
using geometry::optimization::GraphOfConvexSetsOptions;
using geometry::optimization::HPolyhedron;
using geometry::optimization::Point;
using solvers::Binding;
using solvers::ConcatenateVariableRefList;
using solvers::Constraint;
using solvers::Cost;
using solvers::L2NormCost;
using solvers::LinearConstraint;
using solvers::LinearCost;
using solvers::LinearEqualityConstraint;
using symbolic::DecomposeLinearExpressions;
using symbolic::Expression;
using symbolic::MakeMatrixContinuousVariable;
using symbolic::MakeVectorContinuousVariable;
using trajectories::BezierCurve;
using trajectories::CompositeTrajectory;
using trajectories::Trajectory;
using Vertex = GraphOfConvexSets::Vertex;
using Edge = GraphOfConvexSets::Edge;
using VertexId = GraphOfConvexSets::VertexId;
using EdgeId = GraphOfConvexSets::EdgeId;

const double kInf = std::numeric_limits<double>::infinity();

namespace {
using VectorXb = Eigen::Matrix<bool, 1, Eigen::Dynamic>;

// Given a list of matrices, return the matrices with every column where all
// of the matrices are zero in that column, along with a boolean vector
// indicating which columns were preserved (true) or removed (false).
std::tuple<std::vector<MatrixXd>, VectorXb> CondenseToNonzeroColumns(
    std::vector<MatrixXd> matrices) {
  // Validate inputs.
  DRAKE_DEMAND(matrices.size() > 0);
  const int num_cols = matrices[0].cols();
  for (const MatrixXd& matrix : matrices) {
    DRAKE_DEMAND(matrix.cols() == num_cols);
  }

  // Find non-zero columns.
  VectorXb nonzero_cols_mask = VectorXb::Constant(num_cols, false);
  for (const MatrixXd& matrix : matrices) {
    nonzero_cols_mask += matrix.cast<bool>().colwise().any();
  }
  const int nonzero_cols_count = nonzero_cols_mask.count();

  // Create the output, copying only the non-zero columns.
  std::vector<MatrixXd> condensed_matrices;
  for (const MatrixXd& matrix : matrices) {
    MatrixXd& condensed_matrix =
        condensed_matrices.emplace_back(matrix.rows(), nonzero_cols_count);
    int condensed_col = 0;
    for (int orig_col = 0; orig_col < matrix.cols(); ++orig_col) {
      if (nonzero_cols_mask(orig_col)) {
        condensed_matrix.col(condensed_col) = matrix.col(orig_col);
        condensed_col++;
      }
    }
  }
  return std::make_tuple(condensed_matrices, nonzero_cols_mask);
}

// Filters variables given a vector of variables along with a boolean vector
// indicating which rows were preserved (true) or removed (false).
VectorX<symbolic::Variable> FilterVariables(
    const VectorX<symbolic::Variable>& vars,
    const VectorXb& nonzero_cols_mask) {
  VectorX<symbolic::Variable> vars_dense(nonzero_cols_mask.count());
  int row = 0;
  for (int i = 0; i < vars.size(); ++i) {
    if (nonzero_cols_mask(i)) {
      vars_dense(row++) = vars(i);
    }
  }
  return vars_dense;
}
}  // namespace

Subgraph::Subgraph(
    const ConvexSets& regions,
    const std::vector<std::pair<int, int>>& edges_between_regions, int order,
    double h_min, double h_max, std::string name,
    GcsTrajectoryOptimization* traj_opt)
    : regions_(regions),
      order_(order),
      h_min_(h_min),
      name_(std::move(name)),
      traj_opt_(*traj_opt) {
  DRAKE_THROW_UNLESS(order >= 0);
  DRAKE_THROW_UNLESS(!regions_.empty());

  // Make sure all regions have the same ambient dimension.
  for (const std::unique_ptr<ConvexSet>& region : regions_) {
    DRAKE_THROW_UNLESS(region != nullptr);
    DRAKE_THROW_UNLESS(region->ambient_dimension() == num_positions());
  }
  // Make time scaling set once to avoid many allocations when adding the
  // vertices to GCS.
  const HPolyhedron time_scaling_set =
      HPolyhedron::MakeBox(Vector1d(h_min), Vector1d(h_max));

  // Allocating variables and control points to be used in the constraints.
  // Bindings allow formulating the constraints once, and then pass them to all
  // the edges.
  // An edge goes from the vertex u to the vertex v. Where its control points
  // and trajectories are needed for continuity constraints. Saving the
  // variables for u simplifies the cost/constraint formulation for optional
  // costs like the path length cost.
  const MatrixX<symbolic::Variable> u_control =
      MakeMatrixContinuousVariable(num_positions(), order_ + 1, "xu");
  const MatrixX<symbolic::Variable> v_control =
      MakeMatrixContinuousVariable(num_positions(), order_ + 1, "xv");
  const Eigen::Map<const VectorX<symbolic::Variable>> u_control_vars(
      u_control.data(), u_control.size());
  const Eigen::Map<const VectorX<symbolic::Variable>> v_control_vars(
      v_control.data(), v_control.size());

  u_h_ = MakeVectorContinuousVariable(1, "hu");
  const VectorX<symbolic::Variable> v_h = MakeVectorContinuousVariable(1, "hv");

  u_vars_ = solvers::ConcatenateVariableRefList({u_control_vars, u_h_});
  const VectorX<symbolic::Variable> edge_vars =
      solvers::ConcatenateVariableRefList(
          {u_control_vars, u_h_, v_control_vars, v_h});

  u_r_trajectory_ = BezierCurve<Expression>(0, 1, u_control.cast<Expression>());

  const auto v_r_trajectory =
      BezierCurve<Expression>(0, 1, v_control.cast<Expression>());

  // TODO(wrangelvid) Pull this out into a function once we have a better way to
  // extract M from bezier curves.
  const VectorX<Expression> path_continuity_error =
      v_r_trajectory.control_points().col(0) -
      u_r_trajectory_.control_points().col(order);
  MatrixXd M(num_positions(), edge_vars.size());
  DecomposeLinearExpressions(path_continuity_error, edge_vars, &M);
  // Condense M to only keep non-zero columns.
  const auto& [condensed_matrices, nonzero_cols_mask] =
      CondenseToNonzeroColumns({M});
  MatrixXd M_dense = condensed_matrices[0];

  const auto path_continuity_constraint =
      std::make_shared<LinearEqualityConstraint>(
          M_dense, VectorXd::Zero(num_positions()));

  // Add Regions with time scaling set.
  for (size_t i = 0; i < regions_.size(); ++i) {
    ConvexSets vertex_set;
    // Assign each control point to a separate set.
    const int num_points = order + 1;
    vertex_set.reserve(num_points + 1);
    vertex_set.insert(vertex_set.begin(), num_points,
                      ConvexSets::value_type{*regions_[i]});
    // Add time scaling set.
    vertex_set.emplace_back(time_scaling_set);

    vertices_.emplace_back(traj_opt_.gcs_.AddVertex(
        CartesianProduct(vertex_set), fmt::format("{}: {}", name, i)));
    traj_opt->vertex_to_subgraph_[vertices_.back()] = this;
  }

  // Connect vertices with edges.
  for (const auto& [u_index, v_index] : edges_between_regions) {
    // Add edge.
    const Vertex& u = *vertices_[u_index];
    const Vertex& v = *vertices_[v_index];
    Edge* uv_edge = traj_opt_.AddEdge(u, v);

    edges_.emplace_back(uv_edge);

    // Add path continuity constraints.
    uv_edge->AddConstraint(Binding<Constraint>(
        path_continuity_constraint,
        FilterVariables(ConcatenateVariableRefList({u.x(), v.x()}),
                        nonzero_cols_mask)));
  }
}

Subgraph::~Subgraph() = default;

void Subgraph::AddTimeCost(double weight) {
  // The time cost is the sum of duration variables ∑ hᵢ
  auto time_cost =
      std::make_shared<LinearCost>(weight * Eigen::VectorXd::Ones(1), 0.0);

  for (Vertex* v : vertices_) {
    // The duration variable is the last element of the vertex.
    v->AddCost(Binding<LinearCost>(time_cost, v->x().tail(1)));
  }
}

void Subgraph::AddPathLengthCost(const MatrixXd& weight_matrix) {
  /*
    We will upper bound the trajectory length by the sum of the distances
    between the control points. ∑ ||rᵢ − rᵢ₊₁||₂

    In the case of a Bézier curve, the path length is given by the integral of
    the norm of the derivative of the curve.

    So the previous upper bound is equivalent to: ∑ ||ṙᵢ||₂ / order
    Because ||ṙᵢ||₂ = ||rᵢ₊₁ − rᵢ||₂ * order
  */
  DRAKE_THROW_UNLESS(weight_matrix.rows() == num_positions());
  DRAKE_THROW_UNLESS(weight_matrix.cols() == num_positions());

  if (order() == 0) {
    throw std::runtime_error(
        "Path length cost is not defined for a set of order 0.");
  }

  const MatrixX<Expression> u_rdot_control =
      dynamic_pointer_cast_or_throw<BezierCurve<Expression>>(
          u_r_trajectory_.MakeDerivative())
          ->control_points();

  for (int i = 0; i < u_rdot_control.cols(); ++i) {
    MatrixXd M(num_positions(), u_vars_.size());
    DecomposeLinearExpressions(u_rdot_control.col(i) / order(), u_vars_, &M);
    // Condense M to only keep non-zero columns.
    const auto& [condensed_matrices, nonzero_cols_mask] =
        CondenseToNonzeroColumns({M});
    MatrixXd M_dense = condensed_matrices[0];

    const auto path_length_cost = std::make_shared<L2NormCost>(
        weight_matrix * M_dense, VectorXd::Zero(num_positions()));

    for (Vertex* v : vertices_) {
      // The duration variable is the last element of the vertex.
      v->AddCost(Binding<L2NormCost>(
          path_length_cost, FilterVariables(v->x(), nonzero_cols_mask)));
    }
  }
}

void Subgraph::AddPathLengthCost(double weight) {
  const MatrixXd weight_matrix =
      weight * MatrixXd::Identity(num_positions(), num_positions());
  return Subgraph::AddPathLengthCost(weight_matrix);
}

void Subgraph::AddVelocityBounds(const Eigen::Ref<const VectorXd>& lb,
                                 const Eigen::Ref<const VectorXd>& ub) {
  DRAKE_THROW_UNLESS(lb.size() == num_positions());
  DRAKE_THROW_UNLESS(ub.size() == num_positions());
  if (order() == 0) {
    throw std::runtime_error(
        "Velocity Bounds are not defined for a set of order 0.");
  }

  // We have q̇(t) = drds * dsdt = ṙ(s) / h, and h >= 0, so we
  // use h * lb <= ṙ(s) <= h * ub, formulated as:
  // - inf <=   h * lb - ṙ(s) <= 0
  // - inf <= - h * ub + ṙ(s) <= 0

  // This also leverages the convex hull property of the B-splines: if all of
  // the control points satisfy these convex constraints and the curve is
  // inside the convex hull of these constraints, then the curve satisfies the
  // constraints for all t.

  const MatrixX<Expression> u_rdot_control =
      dynamic_pointer_cast_or_throw<BezierCurve<Expression>>(
          u_r_trajectory_.MakeDerivative())
          ->control_points();

  MatrixXd b(u_h_.rows(), u_vars_.size());
  DecomposeLinearExpressions(u_h_.cast<Expression>(), u_vars_, &b);

  for (int i = 0; i < u_rdot_control.cols(); ++i) {
    MatrixXd M(num_positions(), u_vars_.size());
    DecomposeLinearExpressions(u_rdot_control.col(i), u_vars_, &M);
    // Condense M and b to only keep non-zero columns.
    const auto& [condensed_matrices, nonzero_cols_mask] =
        CondenseToNonzeroColumns({M, b});
    MatrixXd M_dense = condensed_matrices[0];
    MatrixXd b_dense = condensed_matrices[1];

    MatrixXd H(2 * num_positions(), nonzero_cols_mask.count());
    H << M_dense - ub * b_dense, -M_dense + lb * b_dense;

    const auto velocity_constraint = std::make_shared<LinearConstraint>(
        H, VectorXd::Constant(2 * num_positions(), -kInf),
        VectorXd::Zero(2 * num_positions()));

    for (Vertex* v : vertices_) {
      v->AddConstraint(Binding<LinearConstraint>(
          velocity_constraint, FilterVariables(v->x(), nonzero_cols_mask)));
    }
  }
}

EdgesBetweenSubgraphs::EdgesBetweenSubgraphs(
    const Subgraph& from_subgraph, const Subgraph& to_subgraph,
    const ConvexSet* subspace, GcsTrajectoryOptimization* traj_opt)
    : traj_opt_(*traj_opt),
      from_subgraph_order_(from_subgraph.order()),
      to_subgraph_order_(to_subgraph.order()) {
  // Formulate edge costs and constraints.
  if (subspace != nullptr) {
    if (subspace->ambient_dimension() != num_positions()) {
      throw std::runtime_error(
          "Subspace dimension must match the number of positions.");
    }
    if (typeid(*subspace) != typeid(Point) &&
        typeid(*subspace) != typeid(HPolyhedron)) {
      throw std::runtime_error("Subspace must be a Point or HPolyhedron.");
    }
  }

  // Allocating variables and control points to be used in the constraints.
  // Bindings allow formulating the constraints once, and then pass
  // them to all the edges.
  // An edge goes from the vertex u to the vertex v. Where its control points
  // and trajectories are needed for continuity constraints. Saving the
  // variables for u simplifies the cost/constraint formulation for optional
  // constraints like the velocity bounds.

  const MatrixX<symbolic::Variable> u_control = MakeMatrixContinuousVariable(
      num_positions(), from_subgraph.order() + 1, "xu");
  const MatrixX<symbolic::Variable> v_control = MakeMatrixContinuousVariable(
      num_positions(), to_subgraph.order() + 1, "xv");
  Eigen::Map<const VectorX<symbolic::Variable>> u_control_vars(
      u_control.data(), u_control.size());
  Eigen::Map<const VectorX<symbolic::Variable>> v_control_vars(
      v_control.data(), v_control.size());

  u_h_ = MakeVectorContinuousVariable(1, "Tu");
  v_h_ = MakeVectorContinuousVariable(1, "Tv");

  u_vars_ = solvers::ConcatenateVariableRefList({u_control_vars, u_h_});
  v_vars_ = solvers::ConcatenateVariableRefList({v_control_vars, v_h_});
  const VectorX<symbolic::Variable> edge_vars =
      solvers::ConcatenateVariableRefList(
          {u_control_vars, u_h_, v_control_vars, v_h_});

  u_r_trajectory_ = BezierCurve<Expression>(0, 1, u_control.cast<Expression>());

  v_r_trajectory_ = BezierCurve<Expression>(0, 1, v_control.cast<Expression>());

  // Zeroth order continuity constraints.
  // TODO(wrangelvid) Pull this out into a function once we have a better way to
  // extract M from bezier curves.
  const VectorX<Expression> path_continuity_error =
      v_r_trajectory_.control_points().col(0) -
      u_r_trajectory_.control_points().col(from_subgraph.order());
  MatrixXd M(num_positions(), edge_vars.size());
  DecomposeLinearExpressions(path_continuity_error, edge_vars, &M);
  // Condense M to only keep non-zero columns.
  const auto& [condensed_matrices, nonzero_cols_mask] =
      CondenseToNonzeroColumns({M});
  MatrixXd M_dense = condensed_matrices[0];

  const auto path_continuity_constraint =
      std::make_shared<LinearEqualityConstraint>(
          M_dense, VectorXd::Zero(num_positions()));

  // TODO(wrangelvid) this can be parallelized.
  for (int i = 0; i < from_subgraph.size(); ++i) {
    for (int j = 0; j < to_subgraph.size(); ++j) {
      if (from_subgraph.regions()[i]->IntersectsWith(
              *to_subgraph.regions()[j])) {
        if (subspace != nullptr) {
          // Check if the regions are connected through the subspace.
          if (!RegionsConnectThroughSubspace(*from_subgraph.regions()[i],
                                             *to_subgraph.regions()[j],
                                             *subspace)) {
            continue;
          }
        }

        // Add edge.
        const Vertex& u = *from_subgraph.vertices_[i];
        const Vertex& v = *to_subgraph.vertices_[j];
        Edge* uv_edge = traj_opt_.AddEdge(u, v);
        edges_.emplace_back(uv_edge);

        // Add path continuity constraints.
        uv_edge->AddConstraint(Binding<LinearEqualityConstraint>(
            path_continuity_constraint,
            FilterVariables(ConcatenateVariableRefList({u.x(), v.x()}),
                            nonzero_cols_mask)));

        if (subspace != nullptr) {
          // Add subspace constraints to the first control point of the v
          // vertex. Since we are using zeroth order continuity, the last
          // control point
          const auto vars = v.x().segment(0, num_positions());
          solvers::MathematicalProgram prog;
          const VectorX<symbolic::Variable> x =
              prog.NewContinuousVariables(num_positions(), "x");
          subspace->AddPointInSetConstraints(&prog, x);
          for (const auto& binding : prog.GetAllConstraints()) {
            const std::shared_ptr<Constraint>& constraint = binding.evaluator();
            uv_edge->AddConstraint(Binding<Constraint>(constraint, vars));
          }
        }
      }
    }
  }
}

EdgesBetweenSubgraphs::~EdgesBetweenSubgraphs() = default;

bool EdgesBetweenSubgraphs::RegionsConnectThroughSubspace(
    const ConvexSet& A, const ConvexSet& B, const ConvexSet& subspace) {
  DRAKE_THROW_UNLESS(A.ambient_dimension() > 0);
  DRAKE_THROW_UNLESS(A.ambient_dimension() == B.ambient_dimension());
  DRAKE_THROW_UNLESS(A.ambient_dimension() == subspace.ambient_dimension());
  if (std::optional<VectorXd> subspace_point = subspace.MaybeGetPoint()) {
    // If the subspace is a point, then the point must be in both A and B.
    return A.PointInSet(*subspace_point) && B.PointInSet(*subspace_point);
  } else {
    // Otherwise, we can formulate a problem to check if a point is contained in
    // A, B and the subspace.
    solvers::MathematicalProgram prog;
    const VectorX<symbolic::Variable> x =
        prog.NewContinuousVariables(num_positions(), "x");
    A.AddPointInSetConstraints(&prog, x);
    B.AddPointInSetConstraints(&prog, x);
    subspace.AddPointInSetConstraints(&prog, x);
    solvers::MathematicalProgramResult result = solvers::Solve(prog);
    return result.is_success();
  }
}

void EdgesBetweenSubgraphs::AddVelocityBounds(
    const Eigen::Ref<const VectorXd>& lb,
    const Eigen::Ref<const VectorXd>& ub) {
  DRAKE_THROW_UNLESS(lb.size() == num_positions());
  DRAKE_THROW_UNLESS(ub.size() == num_positions());

  // We have q̇(t) = drds * dsdt = ṙ(s) / h, and h >= 0, so we
  // use h * lb <= ṙ(s) <= h * ub, formulated as:
  // - inf <=   h * lb - ṙ(s) <= 0
  // - inf <= - h * ub + ṙ(s) <= 0

  // We will enforce the velocity bounds on the last control point of the u set
  // and on the first control point of the v set unless one of the sets are of
  // order zero. In the zero order case, velocity doesn't matter since its a
  // point.

  if (from_subgraph_order_ == 0 && to_subgraph_order_ == 0) {
    throw std::runtime_error(
        "Cannot add velocity bounds to a subgraph edges where both subgraphs "
        "have zero order.");
  }

  if (from_subgraph_order_ > 0) {
    // Add velocity bounds to the last control point of the u set.

    const MatrixX<Expression> u_rdot_control =
        dynamic_pointer_cast_or_throw<BezierCurve<Expression>>(
            u_r_trajectory_.MakeDerivative())
            ->control_points();

    MatrixXd b(u_h_.rows(), u_vars_.size());
    DecomposeLinearExpressions(u_h_.cast<Expression>(), u_vars_, &b);

    // Last control point velocity constraint.
    MatrixXd M(num_positions(), u_vars_.size());
    DecomposeLinearExpressions(u_rdot_control.col(u_rdot_control.cols() - 1),
                               u_vars_, &M);
    // Condense M and b to only keep non-zero columns.
    const auto& [condensed_matrices, nonzero_cols_mask] =
        CondenseToNonzeroColumns({M, b});
    MatrixXd M_dense = condensed_matrices[0];
    MatrixXd b_dense = condensed_matrices[1];

    MatrixXd H(2 * num_positions(), nonzero_cols_mask.count());
    H << M_dense - ub * b_dense, -M_dense + lb * b_dense;

    const auto last_ctrl_pt_velocity_constraint =
        std::make_shared<LinearConstraint>(
            H, VectorXd::Constant(2 * num_positions(), -kInf),
            VectorXd::Zero(2 * num_positions()));

    for (Edge* edge : edges_) {
      edge->AddConstraint(Binding<LinearConstraint>(
          last_ctrl_pt_velocity_constraint,
          FilterVariables(edge->xu(), nonzero_cols_mask)));
    }
  }

  if (to_subgraph_order_ > 0) {
    // Add velocity bounds to the first control point of the v set.
    const MatrixX<Expression> v_rdot_control =
        dynamic_pointer_cast_or_throw<BezierCurve<Expression>>(
            v_r_trajectory_.MakeDerivative())
            ->control_points();

    MatrixXd b(v_h_.rows(), v_vars_.size());
    DecomposeLinearExpressions(v_h_.cast<Expression>(), v_vars_, &b);

    // First control point velocity constraint.
    MatrixXd M(num_positions(), v_vars_.size());
    DecomposeLinearExpressions(v_rdot_control.col(0), v_vars_, &M);
    // Condense M and b to only keep non-zero columns.
    const auto& [condensed_matrices, nonzero_cols_mask] =
        CondenseToNonzeroColumns({M, b});
    MatrixXd M_dense = condensed_matrices[0];
    MatrixXd b_dense = condensed_matrices[1];

    MatrixXd H(2 * num_positions(), nonzero_cols_mask.count());
    H << M_dense - ub * b_dense, -M_dense + lb * b_dense;

    const auto first_ctrl_pt_velocity_constraint =
        std::make_shared<LinearConstraint>(
            H, VectorXd::Constant(2 * num_positions(), -kInf),
            VectorXd::Zero(2 * num_positions()));

    for (Edge* edge : edges_) {
      edge->AddConstraint(Binding<LinearConstraint>(
          first_ctrl_pt_velocity_constraint,
          FilterVariables(edge->xv(), nonzero_cols_mask)));
    }
  }
}

GcsTrajectoryOptimization::GcsTrajectoryOptimization(int num_positions)
    : num_positions_(num_positions) {
  DRAKE_THROW_UNLESS(num_positions >= 1);
}

GcsTrajectoryOptimization::~GcsTrajectoryOptimization() = default;

Subgraph& GcsTrajectoryOptimization::AddRegions(
    const ConvexSets& regions,
    const std::vector<std::pair<int, int>>& edges_between_regions, int order,
    double h_min, double h_max, std::string name) {
  Subgraph* subgraph = new Subgraph(regions, edges_between_regions, order,
                                    h_min, h_max, std::move(name), this);

  // Add global costs to the subgraph.
  for (double weight : global_time_costs_) {
    subgraph->AddTimeCost(weight);
  }

  if (order > 0) {
    // These costs and constraints rely on the derivative of the trajectory.
    for (const MatrixXd& weight_matrix : global_path_length_costs_) {
      subgraph->AddPathLengthCost(weight_matrix);
    }
    for (const auto& [lb, ub] : global_velocity_bounds_) {
      subgraph->AddVelocityBounds(lb, ub);
    }
  }
  return *subgraphs_.emplace_back(subgraph);
}

Subgraph& GcsTrajectoryOptimization::AddRegions(const ConvexSets& regions,
                                                int order, double h_min,
                                                double h_max,
                                                std::string name) {
  if (name.empty()) {
    name = fmt::format("S{}", subgraphs_.size());
  }
  // TODO(wrangelvid): This is O(n^2) and can be improved.
  std::vector<std::pair<int, int>> edges_between_regions;
  for (size_t i = 0; i < regions.size(); ++i) {
    for (size_t j = i + 1; j < regions.size(); ++j) {
      if (regions[i]->IntersectsWith(*regions[j])) {
        // Regions are overlapping, add edge.
        edges_between_regions.emplace_back(i, j);
        edges_between_regions.emplace_back(j, i);
      }
    }
  }

  return GcsTrajectoryOptimization::AddRegions(
      regions, edges_between_regions, order, h_min, h_max, std::move(name));
}

EdgesBetweenSubgraphs& GcsTrajectoryOptimization::AddEdges(
    const Subgraph& from_subgraph, const Subgraph& to_subgraph,
    const ConvexSet* subspace) {
  return *subgraph_edges_.emplace_back(
      new EdgesBetweenSubgraphs(from_subgraph, to_subgraph, subspace, this));
}

void GcsTrajectoryOptimization::AddTimeCost(double weight) {
  // Add time cost to each subgraph.
  for (std::unique_ptr<Subgraph>& subgraph : subgraphs_) {
    subgraph->AddTimeCost(weight);
  }
  global_time_costs_.push_back(weight);
}

void GcsTrajectoryOptimization::AddPathLengthCost(
    const MatrixXd& weight_matrix) {
  // Add path length cost to each subgraph.
  for (std::unique_ptr<Subgraph>& subgraph : subgraphs_) {
    if (subgraph->order() > 0) {
      subgraph->AddPathLengthCost(weight_matrix);
    }
  }
  global_path_length_costs_.push_back(weight_matrix);
}

void GcsTrajectoryOptimization::AddPathLengthCost(double weight) {
  const MatrixXd weight_matrix =
      weight * MatrixXd::Identity(num_positions(), num_positions());
  return GcsTrajectoryOptimization::AddPathLengthCost(weight_matrix);
}

void GcsTrajectoryOptimization::AddVelocityBounds(
    const Eigen::Ref<const VectorXd>& lb,
    const Eigen::Ref<const VectorXd>& ub) {
  DRAKE_THROW_UNLESS(lb.size() == num_positions());
  DRAKE_THROW_UNLESS(ub.size() == num_positions());
  // Add path velocity bounds to each subgraph.
  for (std::unique_ptr<Subgraph>& subgraph : subgraphs_) {
    if (subgraph->order() > 0) {
      subgraph->AddVelocityBounds(lb, ub);
    }
  }
  global_velocity_bounds_.push_back({lb, ub});
}

std::pair<CompositeTrajectory<double>, solvers::MathematicalProgramResult>
GcsTrajectoryOptimization::SolvePath(
    const Subgraph& source, const Subgraph& target,
    const GraphOfConvexSetsOptions& specified_options) {
  // Fill in default options. Note: if these options change, they must also be
  // updated in the method documentation.
  GraphOfConvexSetsOptions options = specified_options;
  if (!options.convex_relaxation) {
    options.convex_relaxation = true;
  }
  if (!options.preprocessing) {
    options.preprocessing = true;
  }
  if (!options.max_rounded_paths) {
    options.max_rounded_paths = 5;
  }

  const VectorXd empty_vector;

  VertexId source_id = source.vertices_[0]->id();
  Vertex* dummy_source = nullptr;

  VertexId target_id = target.vertices_[0]->id();
  Vertex* dummy_target = nullptr;

  if (source.size() != 1) {
    // Source subgraph has more than one region. Add a dummy source vertex.
    dummy_source = gcs_.AddVertex(Point(empty_vector), "Dummy source");
    source_id = dummy_source->id();
    for (const Vertex* v : source.vertices_) {
      AddEdge(*dummy_source, *v);
    }
  }
  const ScopeExit cleanup_dummy_source_before_returning([&]() {
    if (dummy_source != nullptr) {
      gcs_.RemoveVertex(dummy_source->id());
    }
  });

  if (target.size() != 1) {
    // Target subgraph has more than one region. Add a dummy target vertex.
    dummy_target = gcs_.AddVertex(Point(empty_vector), "Dummy target");
    target_id = dummy_target->id();
    for (const Vertex* v : target.vertices_) {
      AddEdge(*v, *dummy_target);
    }
  }
  const ScopeExit cleanup_dummy_target_before_returning([&]() {
    if (dummy_target != nullptr) {
      gcs_.RemoveVertex(dummy_target->id());
    }
  });

  solvers::MathematicalProgramResult result =
      gcs_.SolveShortestPath(source_id, target_id, options);
  if (!result.is_success()) {
    return {CompositeTrajectory<double>({}), result};
  }

  // Extract the flow from the solution.
  std::unordered_map<VertexId, std::vector<Edge*>> outgoing_edges;
  std::unordered_map<EdgeId, double> flows;
  for (Edge* edge : gcs_.Edges()) {
    outgoing_edges[edge->u().id()].push_back(edge);
    flows[edge->id()] = result.GetSolution(edge->phi());
  }

  // Extract the path by traversing the graph with a depth first search.
  std::unordered_set<VertexId> visited_vertex_ids{source_id};
  std::vector<VertexId> path_vertex_ids{source_id};
  std::vector<Edge*> path_edges;
  while (path_vertex_ids.back() != target_id) {
    // Find the edge with the maximum flow from the current node.
    double maximum_flow = 0;
    VertexId max_flow_vertex_id;
    Edge* max_flow_edge = nullptr;
    for (Edge* e : outgoing_edges[path_vertex_ids.back()]) {
      const double next_flow = flows[e->id()];
      const VertexId next_vertex_id = e->v().id();

      // If the edge has not been visited and has a flow greater than the
      // current maximum, update the maximum flow and the vertex id.
      if (visited_vertex_ids.count(e->v().id()) == 0 &&
          next_flow > maximum_flow && next_flow > options.flow_tolerance) {
        maximum_flow = next_flow;
        max_flow_vertex_id = next_vertex_id;
        max_flow_edge = e;
      }
    }

    if (max_flow_edge == nullptr) {
      // If no candidate edges are found, backtrack to the previous node and
      // continue the search.
      path_vertex_ids.pop_back();
      DRAKE_DEMAND(!path_vertex_ids.empty());
      continue;
    } else {
      // If the maximum flow is non-zero, add the vertex to the path and
      // continue the search.
      visited_vertex_ids.insert(max_flow_vertex_id);
      path_vertex_ids.push_back(max_flow_vertex_id);
      path_edges.push_back(max_flow_edge);
    }
  }

  // Remove the dummy edges from the path.
  if (dummy_source != nullptr) {
    DRAKE_DEMAND(!path_edges.empty());
    path_edges.erase(path_edges.begin());
  }
  if (dummy_target != nullptr) {
    DRAKE_DEMAND(!path_edges.empty());
    path_edges.erase(path_edges.end() - 1);
  }

  // Extract the path from the edges.
  std::vector<copyable_unique_ptr<Trajectory<double>>> bezier_curves;
  for (Edge* edge : path_edges) {
    // Extract phi from the solution to rescale the control points and duration
    // in case we get the relaxed solution.
    const double phi_inv = 1 / result.GetSolution(edge->phi());
    // Extract the control points from the solution.
    const int num_control_points = vertex_to_subgraph_[&edge->u()]->order() + 1;
    const MatrixX<double> edge_path_points =
        phi_inv *
        Eigen::Map<MatrixX<double>>(result.GetSolution(edge->xu()).data(),
                                    num_positions(), num_control_points);

    // Extract the duration from the solution.
    double h = phi_inv * result.GetSolution(edge->xu()).tail<1>().value();
    const double start_time =
        bezier_curves.empty() ? 0 : bezier_curves.back()->end_time();

    // Skip edges with a single control point that spend near zero time in the
    // region, since zero order continuity constraint is sufficient. These edges
    // would result in a discontinuous trajectory for velocities and higher
    // derivatives.
    if (!(num_control_points == 1 &&
          vertex_to_subgraph_[&edge->u()]->h_min_ == 0)) {
      bezier_curves.emplace_back(std::make_unique<BezierCurve<double>>(
          start_time, start_time + h, edge_path_points));
    }
  }

  // Get the final control points from the solution.
  const double phi_inv = 1 / result.GetSolution(path_edges.back()->phi());
  const int num_control_points =
      vertex_to_subgraph_[&path_edges.back()->v()]->order() + 1;
  const MatrixX<double> edge_path_points =
      phi_inv * Eigen::Map<MatrixX<double>>(
                    result.GetSolution(path_edges.back()->xv()).data(),
                    num_positions(), num_control_points);

  double h =
      phi_inv * result.GetSolution(path_edges.back()->xv()).tail<1>().value();
  const double start_time =
      bezier_curves.empty() ? 0 : bezier_curves.back()->end_time();

  // Skip edges with a single control point that spend near zero time in the
  // region, since zero order continuity constraint is sufficient.
  if (!(num_control_points == 1 &&
        vertex_to_subgraph_[&path_edges.back()->v()]->h_min_ == 0)) {
    bezier_curves.emplace_back(std::make_unique<BezierCurve<double>>(
        start_time, start_time + h, edge_path_points));
  }

  return {CompositeTrajectory<double>(bezier_curves), result};
}

Edge* GcsTrajectoryOptimization::AddEdge(const Vertex& u, const Vertex& v) {
  return gcs_.AddEdge(u, v, fmt::format("{} -> {}", u.name(), v.name()));
}

double GcsTrajectoryOptimization::EstimateComplexity() const {
  double result = 0;
  // TODO(ggould) A more correct computation estimate would be:
  // If each vertex and edge problem were solved only once, the cost would be
  // | constraint_var_sum = variables per constraint summed over constraints
  // | cost_vars = total unique variables over all costs
  // | constraint_vars = total unique variables over all constraints
  // : constraint_var_sum * cost_vars * constraint_vars^2
  // In fact each vertex must be solved at least as many times as it has
  // edges, so multiply the vertex cost by the vertex's arity.
  for (const auto* v : gcs_.Vertices()) {
    for (const auto& c : v->GetCosts()) {
      result += c.GetNumElements();
    }
    for (const auto& c : v->GetConstraints()) {
      result += c.GetNumElements();
    }
  }
  for (const auto* e : gcs_.Edges()) {
    for (const auto& c : e->GetCosts()) {
      result += c.GetNumElements();
    }
    for (const auto& c : e->GetConstraints()) {
      result += c.GetNumElements();
    }
  }
  return result;
}

}  // namespace trajectory_optimization
}  // namespace planning
}  // namespace drake
