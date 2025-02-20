#include "drake/multibody/contact_solvers/minimum_degree_ordering.h"

#include <algorithm>
#include <functional>
#include <set>
#include <utility>

namespace drake {
namespace multibody {
namespace contact_solvers {
namespace internal {

std::vector<int> Union(const std::vector<int>& a, const std::vector<int>& b) {
  std::vector<int> result;
  result.reserve(a.size() + b.size());
  std::set_union(a.begin(), a.end(), b.begin(), b.end(),
                 std::back_inserter(result));
  return result;
}

std::vector<int> SetDifference(const std::vector<int>& a,
                               const std::vector<int>& b) {
  std::vector<int> result;
  result.reserve(a.size());
  std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                      std::back_inserter(result));
  return result;
}

void RemoveValueFromSortedVector(int value, std::vector<int>* sorted_vector) {
  auto it =
      std::lower_bound(sorted_vector->begin(), sorted_vector->end(), value);
  /* Check if the value was found and remove it. */
  while (it != sorted_vector->end() && *it == value) {
    it = sorted_vector->erase(it);
  }
}

void InsertValueInSortedVector(int value, std::vector<int>* sorted_vector) {
  auto it =
      std::lower_bound(sorted_vector->begin(), sorted_vector->end(), value);
  /* Check if the value doesn't already exist and insert it. */
  if (it == sorted_vector->end() || *it != value) {
    sorted_vector->insert(it, value);
  }
}

void Node::UpdateExternalDegree(const std::vector<Node>& nodes) {
  degree = 0;

  /* Add in |Aᵢ \ i| term. */
  for (int a : A) {
    degree += nodes[a].size;
  }

  /* Add in |L \ i| term where L = ∪ₑLₑ and e ∈ Eᵢ. */
  std::vector<int> L_union;
  // TODO(xuchenhan-tri): This can be quadratic in the worst case and can be
  // made more efficient by doing a K-way union if needed.
  for (int e : E) {
    L_union = Union(L_union, nodes[e].L);
  }
  RemoveValueFromSortedVector(index, &L_union);
  for (int l : L_union) {
    degree += nodes[l].size;
  }
}

std::vector<int> ComputeMinimumDegreeOrdering(
    const BlockSparsityPattern& block_sparsity_pattern) {
  /* Initialize for Minimum Degree: Populate index, size, and A, the list of
   adjacent variables. E, L are empty initially. */
  const std::vector<int>& block_sizes = block_sparsity_pattern.block_sizes();
  const int num_nodes = block_sizes.size();
  std::vector<Node> nodes(num_nodes);
  for (int n = 0; n < num_nodes; ++n) {
    nodes[n].index = n;
    nodes[n].size = block_sizes[n];
  }
  for (int n = 0; n < num_nodes; ++n) {
    Node& node = nodes[n];
    const std::vector<int>& neighbors = block_sparsity_pattern.neighbors()[n];
    for (int neighbor : neighbors) {
      if (n != neighbor) {
        /* We modify both `node` and `neighbor` here because for each ij-pair
         only one of (i, j) and (j, i) is recorded in `sparsity pattern` but
         we want j be part of Ai and i to be part of Aj. */
        node.A.push_back(nodes[neighbor].index);
        node.degree += nodes[neighbor].size;
        nodes[neighbor].A.push_back(node.index);
        nodes[neighbor].degree += node.size;
      }
    }
  }
  /* Maintain the invariance that A, E, L vectors in a node are all sorted. */
  for (auto& node : nodes) {
    std::vector<int>& A = node.A;
    /* Only A is non-empty at this point. */
    std::sort(A.begin(), A.end());
  }

  /* The ideal data structure to use here is a priority queue implemeneted by a
   heap. However, the Minimum Degree algorithm requires updating neighboring
   nodes when a node is eliminated and std::priority_queue doesn't support
   modifying existing elements. There are ways to get around it by duplicating
   nodes and ignoring out-of-date nodes, but for simplicity and readability, we
   use an std::set here instead. */
  std::set<IndexDegree> sorted_nodes;
  /* Keep the nodes sorted by degrees and break ties with indices. We use a
   striped down version of the node to keep only the necessary information
   (degree and index). */
  for (int n = 0; n < num_nodes; ++n) {
    IndexDegree node = {.degree = nodes[n].degree, .index = nodes[n].index};
    sorted_nodes.insert(node);
  }

  /* The resulting elimination ordering. */
  std::vector<int> result(num_nodes);
  /* Begin elimination. */
  for (int k = 0; k < num_nodes; ++k) {
    DRAKE_DEMAND(ssize(sorted_nodes) == num_nodes - k);
    const IndexDegree min_node =
        sorted_nodes.extract(sorted_nodes.begin()).value();
    /* p is the variable to be eliminated next. */
    const int p = min_node.index;
    result[k] = p;
    Node& node_p = nodes[p];

    /* Turn node p from a variable to an element. */
    node_p.L = node_p.A;
    /* Absorb into Lp all variables adjacent to element e where e is adjacent to
     p. Make sure to exclude p itself since p is turning from a variable into an
     element. */
    for (int e : node_p.E) {
      node_p.L = Union(node_p.L, nodes[e].L);
    }
    RemoveValueFromSortedVector(p, &(node_p.L));
    /* Update all neighboring variables of p. */
    for (int i : node_p.L) {
      Node& node_i = nodes[i];
      const IndexDegree old_node = {.degree = node_i.degree,
                                    .index = node_i.index};
      /* Pruning. */
      node_i.A = SetDifference(node_i.A, node_p.L);
      /* Convert p from a variable to an element in node i. Note that Lp was
       already updated prior to the loop, and therefore we can now safely
       remove the absorbed elements in node i */
      node_i.E = SetDifference(node_i.E, node_p.E);
      RemoveValueFromSortedVector(p, &(node_i.A));
      InsertValueInSortedVector(p, &(node_i.E));
      /* Compute external degree. */
      node_i.UpdateExternalDegree(nodes);
      IndexDegree new_node = {.degree = node_i.degree, .index = node_i.index};
      sorted_nodes.erase(old_node);
      sorted_nodes.insert(new_node);
    }
    /* Convert node p from a supervariable to an element. */
    node_p.A.clear();
    node_p.E.clear();
  }
  return result;
}

/* The complexity of this symbolic factorization is O(|L|) where L is the
 factorized matrix. */
BlockSparsityPattern SymbolicCholeskyFactor(
    const BlockSparsityPattern& block_sparsity) {
  const std::vector<int>& block_sizes = block_sparsity.block_sizes();
  /* sparsity pattern of the original symmetric matrix. */
  const std::vector<std::vector<int>>& A_sparsity = block_sparsity.neighbors();
  const int N = block_sizes.size();

  /* children[p] stores the children of node p in the elimination tree, in
   increasing order. See [Vandenberghe, 2015] Section 4.3 for the definition
   of an elimination tree.

   [Vandenberghe, 2015] Vandenberghe, Lieven, and Martin S. Andersen. "Chordal
   graphs and semidefinite optimization." Foundations and Trends® in
   Optimization 1.4 (2015): 241-433. */
  std::vector<std::vector<int>> children(N);
  /* L_sparsity[j] denotes the non-zero row blocks of column j. */
  std::vector<std::vector<int>> L_sparsity(N);
  for (int j = 0; j < N; ++j) {
    /* The sparsity pattern for the j-th node in L is the union of the
     neighbors of the j-th node in A and all neighbors of children of the j-th
     node that are larger than j. We first take the union of all neighbors of
     j and children of j and then delete all neighbors that are smaller than
     j. This is the same as described in equation 4.3 in [Davis 2006].
      Lⱼ = Aⱼ ∪ {j} ∪ ( ∪ Lₛ \ {s})
     where the union over s is over all children of j. */
    L_sparsity[j] = A_sparsity[j];
    for (int c : children[j]) {
      // TODO(xuchenhan-tri): Consider doing a k-way union instead of a loop
      // for efficiency.
      L_sparsity[j] = Union(L_sparsity[j], L_sparsity[c]);
    }
    /* Instead of union over Lₛ \ {s}, we union over Lₛ and remove all entries
     smaller than j here in a single pass. */
    L_sparsity[j] = std::vector<int>(
        std::lower_bound(L_sparsity[j].begin(), L_sparsity[j].end(), j),
        L_sparsity[j].end());

    /* Record the parent of j if j isn't already the root. */
    if (L_sparsity[j].size() > 1) {
      /* The 0-th entry is j itself and the 1st entry is j-th parent. See
       [Vandenberghe, 2015] Section 4.3. */
      const int parent_of_j = L_sparsity[j][1];
      children[parent_of_j].emplace_back(j);
    }
  }
  return BlockSparsityPattern(block_sizes, L_sparsity);
}

}  // namespace internal
}  // namespace contact_solvers
}  // namespace multibody
}  // namespace drake
