#include "voxblox/skeletons/nanoflann_interface.h"

#include "voxblox/skeletons/skeleton_generator.h"

namespace voxblox {

SkeletonGenerator::SkeletonGenerator(Layer<EsdfVoxel>* esdf_layer)
    : min_separation_angle_(0.7),
      generate_by_layer_neighbors_(false),
      num_neighbors_for_edge_(18),
      esdf_layer_(esdf_layer) {
  CHECK_NOTNULL(esdf_layer);

  esdf_voxels_per_side_ = esdf_layer_->voxels_per_side();

  // Make a skeleton layer to store the intermediate skeleton steps, along with
  // the lists.
  skeleton_layer_.reset(new Layer<SkeletonVoxel>(
      esdf_layer_->voxel_size(), esdf_layer_->voxels_per_side()));

  // Initialize the template matchers.
  pruning_template_matcher_.setDeletionTemplates();
  corner_template_matcher_.setCornerTemplates();
}

void SkeletonGenerator::generateSkeleton() {
  timing::Timer generate_timer("skeleton/gvd");

  // Clear the skeleton and start over.
  skeleton_.getSkeletonPoints().clear();

  // Iterate over all blocks in the ESDF...
  // So should be wavefront ends, but can just check parent direction.
  // Maybe a minimum angle between parent directions of neighbors?
  BlockIndexList blocks;
  esdf_layer_->getAllAllocatedBlocks(&blocks);

  for (const BlockIndex& block_index : blocks) {
    const Block<EsdfVoxel>::Ptr& esdf_block =
        esdf_layer_->getBlockPtrByIndex(block_index);
    Block<SkeletonVoxel>::Ptr skeleton_block =
        skeleton_layer_->allocateBlockPtrByIndex(block_index);

    const size_t num_voxels_per_block = esdf_block->num_voxels();

    for (size_t lin_index = 0u; lin_index < num_voxels_per_block; ++lin_index) {
      const EsdfVoxel& esdf_voxel =
          esdf_block->getVoxelByLinearIndex(lin_index);
      VoxelIndex voxel_index =
          esdf_block->computeVoxelIndexFromLinearIndex(lin_index);

      if (!esdf_voxel.observed || esdf_voxel.distance < 0.0f ||
          esdf_voxel.fixed) {
        continue;
      }

      Point coords = esdf_block->computeCoordinatesFromVoxelIndex(voxel_index);

      // Get the floating-point distance of this voxel, normalize it as long as
      // it's not 0.
      Eigen::Vector3f parent_dir = esdf_voxel.parent.cast<float>();

      // Parent-less voxel (probably in max-distance area), just skip.
      if (parent_dir.norm() < 1e-6) {
        continue;
      }
      parent_dir.normalize();

      // See what the neighbors are pointing to. You can choose what
      // connectivity you want.
      AlignedVector<VoxelKey> neighbors;
      AlignedVector<float> distances;
      AlignedVector<Eigen::Vector3i> directions;
      getNeighborsAndDistances(block_index, voxel_index, 6, &neighbors,
                               &distances, &directions);

      // Just go though the 6-connectivity set of this to start.
      SkeletonPoint skeleton_point;
      bool on_skeleton = false;
      for (size_t i = 0; i < neighbors.size(); ++i) {
        // Get this voxel with way too many checks.
        // Get the block for this voxel.
        BlockIndex neighbor_block_index = neighbors[i].first;
        VoxelIndex neighbor_voxel_index = neighbors[i].second;
        Block<EsdfVoxel>::Ptr neighbor_block;
        if (neighbor_block_index == block_index) {
          neighbor_block = esdf_block;
        } else {
          neighbor_block =
              esdf_layer_->getBlockPtrByIndex(neighbor_block_index);
        }
        if (!neighbor_block) {
          continue;
        }
        CHECK(neighbor_block->isValidVoxelIndex(neighbor_voxel_index))
            << "Neigbor voxel index: " << neighbor_voxel_index.transpose();

        EsdfVoxel& neighbor_voxel =
            neighbor_block->getVoxelByVoxelIndex(neighbor_voxel_index);

        if (!neighbor_voxel.observed || neighbor_voxel.distance < 0.0f ||
            neighbor_voxel.fixed) {
          continue;
        }

        // Get the relative distance: that is, if this voxel was on top of
        // ours, what direction would it point in, roughly?
        // Make sure this is a double so we can normalize it.
        Eigen::Vector3f relative_direction =
            neighbor_voxel.parent.cast<float>() + directions[i].cast<float>();

        if (relative_direction.norm() < 1e-6) {
          // This is pointing at us! We're its parent. No way is this a skeleton
          // point.
          continue;
        }
        relative_direction.normalize();

        // Compute the dot product between the two...
        float dot_prod = relative_direction.dot(parent_dir);
        if (dot_prod <= min_separation_angle_) {
          // Then this is a ridge or something! Probably. Who knows.
          on_skeleton = true;
          skeleton_point.num_basis_points++;
          skeleton_point.basis_directions.push_back(relative_direction);
          /* printf(
              "Coord: %f %f %f Parent direction: %f %f %f Voxel parent: %f %f "
              "%f "
              "Neighbor direction: %d %d %d "
              "Direction to/from neighbor: %d %d %d Relative direction: %f %f "
              "%f Dot product: %f\n",
              coords.x(), coords.y(), coords.z(), parent_dir.x(),
              parent_dir.y(), parent_dir.z(), voxel_parent.x(),
              voxel_parent.y(), voxel_parent.z(), neighbor_voxel.parent.x(),
              neighbor_voxel.parent.y(), neighbor_voxel.parent.z(),
              directions[i].x(), directions[i].y(), directions[i].z(),
              relative_direction.x(), relative_direction.y(),
              relative_direction.z(), dot_prod); */
        }
      }
      if (on_skeleton) {
        skeleton_point.distance =
            esdf_block->getVoxelByVoxelIndex(voxel_index).distance;

        skeleton_point.point = coords;
        skeleton_.getSkeletonPoints().push_back(skeleton_point);

        // Also add it to the layer.
        SkeletonVoxel& skeleton_voxel =
            skeleton_block->getVoxelByVoxelIndex(voxel_index);
        skeleton_voxel.distance = skeleton_point.distance;
        skeleton_voxel.num_basis_points = skeleton_point.num_basis_points;
        if (!generate_by_layer_neighbors_) {
          skeleton_voxel.is_face = (skeleton_voxel.num_basis_points == 2);
          skeleton_voxel.is_edge = (skeleton_voxel.num_basis_points == 3);
          skeleton_voxel.is_vertex = (skeleton_voxel.num_basis_points == 4);

          if (skeleton_voxel.is_edge) {
            skeleton_.getEdgePoints().push_back(skeleton_point);
          }
          if (skeleton_voxel.is_vertex) {
            skeleton_.getVertexPoints().push_back(skeleton_point);
          }
        } else {
          // If we're generating by layer neighbors, don't bother setting
          // vertex vs. edge yet.
          skeleton_voxel.is_face = true;
        }
        // return;
      }
    }
  }

  if (generate_by_layer_neighbors_) {
    generateEdgesByLayerNeighbors();
    // Keep going until a certain small percentage remains...
    size_t num_pruned = 1;
    while (num_pruned > 0) {
      num_pruned = pruneDiagramEdges();
    }

    generateVerticesByLayerNeighbors();
  }
}

void SkeletonGenerator::generateEdgesByLayerNeighbors() {
  timing::Timer generate_timer("skeleton/neighbor_gvd_edge");

  // Rather than iterate over the entire layer, let's just go over all the
  // points in the skeleton.
  const AlignedVector<SkeletonPoint>& skeleton_points =
      skeleton_.getSkeletonPoints();

  // Then figure out how to connect them to other vertices by following the
  // skeleton layer voxels.
  for (const SkeletonPoint& point : skeleton_points) {
    // Get the voxel.
    BlockIndex block_index =
        skeleton_layer_->computeBlockIndexFromCoordinates(point.point);
    Block<SkeletonVoxel>::Ptr block_ptr;
    block_ptr = skeleton_layer_->getBlockPtrByIndex(block_index);
    CHECK(block_ptr);
    VoxelIndex voxel_index =
        block_ptr->computeVoxelIndexFromCoordinates(point.point);
    SkeletonVoxel& voxel = block_ptr->getVoxelByVoxelIndex(voxel_index);

    // Now just get the neighbors and count how many are on the skeleton.
    AlignedVector<VoxelKey> neighbors;
    AlignedVector<float> distances;
    AlignedVector<Eigen::Vector3i> directions;
    getNeighborsAndDistances(block_index, voxel_index, 26, &neighbors,
                             &distances, &directions);

    int num_neighbors_on_medial_axis = 0;
    for (size_t i = 0; i < neighbors.size(); ++i) {
      // Get the block for this voxel.
      BlockIndex neighbor_block_index = neighbors[i].first;
      VoxelIndex neighbor_voxel_index = neighbors[i].second;
      Block<SkeletonVoxel>::Ptr neighbor_block;
      if (neighbor_block_index == block_index) {
        neighbor_block = block_ptr;
      } else {
        neighbor_block =
            skeleton_layer_->getBlockPtrByIndex(neighbor_block_index);
      }
      if (!neighbor_block) {
        continue;
      }
      SkeletonVoxel& neighbor_voxel =
          neighbor_block->getVoxelByVoxelIndex(neighbor_voxel_index);

      if (neighbor_voxel.is_face) {
        num_neighbors_on_medial_axis++;
      }
    }
    if (num_neighbors_on_medial_axis >= num_neighbors_for_edge_) {
      voxel.is_edge = true;
      skeleton_.getEdgePoints().push_back(point);
    }
  }
}

size_t SkeletonGenerator::pruneDiagramEdges() {
  timing::Timer timer("skeleton/prune_edges");

  // Go through all edge points, checking them against the templates. Remove
  // any that fit the template, and mark them in removal indices.
  std::vector<size_t> removal_indices;

  const AlignedVector<SkeletonPoint>& edge_points = skeleton_.getEdgePoints();

  size_t j = 0;
  for (const SkeletonPoint& edge : edge_points) {
    // Get the voxel.
    BlockIndex block_index =
        skeleton_layer_->computeBlockIndexFromCoordinates(edge.point);
    Block<SkeletonVoxel>::Ptr block_ptr;
    block_ptr = skeleton_layer_->getBlockPtrByIndex(block_index);
    CHECK(block_ptr);
    VoxelIndex voxel_index =
        block_ptr->computeVoxelIndexFromCoordinates(edge.point);
    SkeletonVoxel& voxel = block_ptr->getVoxelByVoxelIndex(voxel_index);
    if (!voxel.is_edge) {
      // this is already deleted.
      continue;
    }

    // Now just get the neighbors and count how many are on the skeleton.
    AlignedVector<VoxelKey> neighbors;
    AlignedVector<float> distances;
    AlignedVector<Eigen::Vector3i> directions;
    getNeighborsAndDistances(block_index, voxel_index, 26, &neighbors,
                             &distances, &directions);

    std::bitset<27> neighbor_bitset;
    for (size_t i = 0; i < neighbors.size(); ++i) {
      // Get the block for this voxel.
      BlockIndex neighbor_block_index = neighbors[i].first;
      VoxelIndex neighbor_voxel_index = neighbors[i].second;
      Block<SkeletonVoxel>::Ptr neighbor_block;
      if (neighbor_block_index == block_index) {
        neighbor_block = block_ptr;
      } else {
        neighbor_block =
            skeleton_layer_->getBlockPtrByIndex(neighbor_block_index);
      }
      if (!neighbor_block) {
        continue;
      }
      SkeletonVoxel& neighbor_voxel =
          neighbor_block->getVoxelByVoxelIndex(neighbor_voxel_index);

      if (neighbor_voxel.is_edge) {
        neighbor_bitset[mapNeighborIndexToBitsetIndex(i)] = true;
      }
    }
    if (pruning_template_matcher_.fitsTemplates(neighbor_bitset)) {
      if (isSimplePoint(neighbor_bitset) && !isEndPoint(neighbor_bitset)) {
        // LOG(INFO) << "Removing edge number " << j;
        voxel.is_edge = false;
        removal_indices.push_back(j);
      } else {
        // LOG(INFO) << "Wanted to remove edge " << j << " but it is not
        // simple.";
      }
    }
    j++;
  }

  size_t num_removed = removal_indices.size();

  AlignedVector<SkeletonPoint>& non_const_edge_points =
      skeleton_.getEdgePoints();
  // They are necessarily already sorted, as we iterate over this vector
  // to build the removal indices.
  std::reverse(removal_indices.begin(), removal_indices.end());

  for (size_t index : removal_indices) {
    non_const_edge_points[index].distance = -1;
    non_const_edge_points.erase(non_const_edge_points.begin() + index);
  }
  return num_removed;
}

size_t SkeletonGenerator::mapNeighborIndexToBitsetIndex(
    size_t neighbor_index) const {
  // This is the mapping between 6-connectivity first, then 18- then 26-
  // to just [0 1 2; 3 4 5; 6 7 8] etc. in 3D.
  switch (neighbor_index) {
    case 24:
      return 0;
    case 12:
      return 1;
    case 20:
      return 2;
    case 15:
      return 3;
    case 4:
      return 4;
    case 14:
      return 5;
    case 22:
      return 6;
    case 10:
      return 7;
    case 18:
      return 8;
    case 9:
      return 9;
    case 3:
      return 10;
    case 7:
      return 11;
    case 1:
      return 12;
    // 13 is skipped since it's the center voxel, and therefore not in the
    // neighbor index.
    case 0:
      return 14;
    case 8:
      return 15;
    case 2:
      return 16;
    case 6:
      return 17;
    case 25:
      return 18;
    case 13:
      return 19;
    case 21:
      return 20;
    case 17:
      return 21;
    case 5:
      return 22;
    case 16:
      return 23;
    case 23:
      return 24;
    case 11:
      return 25;
    case 19:
      return 26;
    default:
      return 13;  // This is the center pixel (not used).
  };
}

void SkeletonGenerator::generateVerticesByLayerNeighbors() {
  timing::Timer generate_timer("skeleton/neighbor_gvd_vertex");

  // Rather than iterate over the entire layer, let's just go over all the
  // points in the skeleton.
  const AlignedVector<SkeletonPoint>& edge_points = skeleton_.getEdgePoints();

  // Then figure out how to connect them to other vertices by following the
  // skeleton layer voxels.
  for (const SkeletonPoint& point : edge_points) {
    // Get the voxel.
    BlockIndex block_index =
        skeleton_layer_->computeBlockIndexFromCoordinates(point.point);
    Block<SkeletonVoxel>::Ptr block_ptr;
    block_ptr = skeleton_layer_->getBlockPtrByIndex(block_index);
    CHECK(block_ptr);
    VoxelIndex voxel_index =
        block_ptr->computeVoxelIndexFromCoordinates(point.point);
    SkeletonVoxel& voxel = block_ptr->getVoxelByVoxelIndex(voxel_index);

    // Now just get the neighbors and count how many are on the skeleton.
    AlignedVector<VoxelKey> neighbors;
    AlignedVector<float> distances;
    AlignedVector<Eigen::Vector3i> directions;
    getNeighborsAndDistances(block_index, voxel_index, 26, &neighbors,
                             &distances, &directions);

    int num_neighbors_on_edges = 0;
    // Just go though the 6-connectivity set of this to start.
    for (size_t i = 0; i < neighbors.size(); ++i) {
      // Get this voxel with way too many checks.
      // Get the block for this voxel.
      BlockIndex neighbor_block_index = neighbors[i].first;
      VoxelIndex neighbor_voxel_index = neighbors[i].second;
      Block<SkeletonVoxel>::Ptr neighbor_block;
      if (neighbor_block_index == block_index) {
        neighbor_block = block_ptr;
      } else {
        neighbor_block =
            skeleton_layer_->getBlockPtrByIndex(neighbor_block_index);
      }
      if (!neighbor_block) {
        continue;
      }
      SkeletonVoxel& neighbor_voxel =
          neighbor_block->getVoxelByVoxelIndex(neighbor_voxel_index);

      if (neighbor_voxel.is_edge) {
        num_neighbors_on_edges++;
      }
    }
    if (num_neighbors_on_edges >= 3 || num_neighbors_on_edges == 1) {
      voxel.is_vertex = true;
      skeleton_.getVertexPoints().push_back(point);
    }
  }
}

void SkeletonGenerator::generateSparseGraph() {
  timing::Timer generate_timer("skeleton/graph");

  graph_.clear();

  // Start with all the vertices.
  // Put them into the graph structure.
  const AlignedVector<SkeletonPoint>& vertex_points =
      skeleton_.getVertexPoints();
  // Store all the IDs we need to iterate over later.
  std::vector<int64_t> vertex_ids;
  vertex_ids.reserve(vertex_points.size());

  for (const SkeletonPoint& point : vertex_points) {
    SkeletonVertex vertex;
    vertex.point = point.point;
    vertex.distance = point.distance;
    int64_t vertex_id = graph_.addVertex(vertex);
    vertex_ids.push_back(vertex_id);

    // Also set the vertex ids in the layer (uuuughhhh)
    Block<SkeletonVoxel>::Ptr block_ptr;
    block_ptr = skeleton_layer_->getBlockPtrByCoordinates(vertex.point);
    CHECK(block_ptr);
    SkeletonVoxel& voxel = block_ptr->getVoxelByCoordinates(vertex.point);
    voxel.vertex_id = vertex_id;
  }

  // Then figure out how to connect them to other vertices by following the
  // skeleton layer voxels.
  for (int64_t vertex_id : vertex_ids) {
    // LOG(INFO) << "Checking vertex id: " << vertex_id;
    SkeletonVertex& vertex = graph_.getVertex(vertex_id);

    // Search the edge map for this guy's neighbors.
    BlockIndex block_index =
        skeleton_layer_->computeBlockIndexFromCoordinates(vertex.point);
    Block<SkeletonVoxel>::Ptr block_ptr;
    block_ptr = skeleton_layer_->getBlockPtrByIndex(block_index);
    CHECK(block_ptr);
    VoxelIndex voxel_index =
        block_ptr->computeVoxelIndexFromCoordinates(vertex.point);

    // Ok get its neighbors and check them all.
    // If one of them is an edge, follow it as far as you can.

    AlignedVector<VoxelKey> neighbors;
    AlignedVector<float> distances;
    AlignedVector<Eigen::Vector3i> directions;
    getNeighborsAndDistances(block_index, voxel_index, 26, &neighbors,
                             &distances, &directions);

    for (size_t i = 0; i < neighbors.size(); ++i) {
      // Get this voxel with way too many checks.
      // Get the block for this voxel.
      BlockIndex neighbor_block_index = neighbors[i].first;
      VoxelIndex neighbor_voxel_index = neighbors[i].second;
      Block<SkeletonVoxel>::Ptr neighbor_block;
      if (neighbor_block_index == block_index) {
        neighbor_block = block_ptr;
      } else {
        neighbor_block =
            skeleton_layer_->getBlockPtrByIndex(neighbor_block_index);
      }
      if (!neighbor_block) {
        continue;
      }
      CHECK(neighbor_block->isValidVoxelIndex(neighbor_voxel_index))
          << "Neigbor voxel index: " << neighbor_voxel_index.transpose();

      SkeletonVoxel& neighbor_voxel =
          neighbor_block->getVoxelByVoxelIndex(neighbor_voxel_index);

      if (!neighbor_voxel.is_edge) {
        continue;
      }
      // We should probably just ignore adjacent vertices anyway, but figure
      // out how to deal with this later.
      // Let's see what we can do by following one edge at a time...
      // For now just take the first vertex it finds...
      int64_t connected_vertex_id = -1;
      float min_distance = 0.0, max_distance = 0.0;
      bool vertex_found =
          followEdge(neighbor_block_index, neighbor_voxel_index, directions[i],
                     &connected_vertex_id, &min_distance, &max_distance);

      if (vertex_found) {
        if (connected_vertex_id == vertex_id) {
          // I am still unclear on how this happens but apparently it does.
          continue;
        }
        // Before adding an edge, make sure it's not already in there...
        bool already_exists = false;
        for (int64_t edge_id : vertex.edge_list) {
          SkeletonEdge& edge = graph_.getEdge(edge_id);
          if (edge.start_vertex == connected_vertex_id ||
              edge.end_vertex == connected_vertex_id) {
            already_exists = true;
            break;
          }
        }
        if (already_exists) {
          continue;
        }

        // Ok it's new, let's add this little guy.
        SkeletonEdge edge;
        edge.start_vertex = vertex_id;
        edge.end_vertex = connected_vertex_id;
        edge.start_distance = min_distance;
        edge.end_distance = max_distance;
        // Start and end are filled in by the addEdge function.
        int64_t edge_id = graph_.addEdge(edge);
        /* LOG(INFO) << "Added an edge (" << edge_id << ") from "
                  << edge.start_vertex << " to " << edge.end_vertex
                  << " with distance: " << min_distance; */
      }
    }
  }
}

// Copied from ESDF integrator... Probably best to factor this out into
// something.
// Uses 26-connectivity and quasi-Euclidean distances.
// Directions is the direction that the neighbor voxel lives in. If you
// need the direction FROM the neighbor voxel TO the current voxel, take
// negative of the given direction.
void SkeletonGenerator::getNeighborsAndDistances(
    const BlockIndex& block_index, const VoxelIndex& voxel_index,
    int connectivity, AlignedVector<VoxelKey>* neighbors,
    AlignedVector<float>* distances,
    AlignedVector<Eigen::Vector3i>* directions) const {
  CHECK_NOTNULL(neighbors);
  CHECK_NOTNULL(distances);
  CHECK_NOTNULL(directions);

  static const double kSqrt2 = std::sqrt(2);
  static const double kSqrt3 = std::sqrt(3);
  static const size_t kNumNeighbors = 26;

  neighbors->reserve(kNumNeighbors);
  distances->reserve(kNumNeighbors);
  directions->reserve(kNumNeighbors);

  VoxelKey neighbor;
  Eigen::Vector3i direction;
  direction.setZero();
  // Distance 1 set.
  for (unsigned int i = 0; i < 3; ++i) {
    for (int j = -1; j <= 1; j += 2) {
      direction(i) = j;
      getNeighbor(block_index, voxel_index, direction, &neighbor.first,
                  &neighbor.second);
      neighbors->emplace_back(neighbor);
      distances->emplace_back(1.0);
      directions->emplace_back(direction);
    }
    direction(i) = 0;
  }
  if (connectivity > 6) {
    // Distance sqrt(2) set.
    for (unsigned int i = 0; i < 3; ++i) {
      unsigned int next_i = (i + 1) % 3;
      for (int j = -1; j <= 1; j += 2) {
        direction(i) = j;
        for (int k = -1; k <= 1; k += 2) {
          direction(next_i) = k;
          getNeighbor(block_index, voxel_index, direction, &neighbor.first,
                      &neighbor.second);
          neighbors->emplace_back(neighbor);
          distances->emplace_back(kSqrt2);
          directions->emplace_back(direction);
        }
        direction(i) = 0;
        direction(next_i) = 0;
      }
    }
  }

  if (connectivity > 18) {
    // Distance sqrt(3) set.
    for (int i = -1; i <= 1; i += 2) {
      direction(0) = i;
      for (int j = -1; j <= 1; j += 2) {
        direction(1) = j;
        for (int k = -1; k <= 1; k += 2) {
          direction(2) = k;
          getNeighbor(block_index, voxel_index, direction, &neighbor.first,
                      &neighbor.second);
          neighbors->emplace_back(neighbor);
          distances->emplace_back(kSqrt3);
          directions->emplace_back(direction);
        }
      }
    }
  }
}

void SkeletonGenerator::getNeighbor(const BlockIndex& block_index,
                                    const VoxelIndex& voxel_index,
                                    const Eigen::Vector3i& direction,
                                    BlockIndex* neighbor_block_index,
                                    VoxelIndex* neighbor_voxel_index) const {
  DCHECK(neighbor_block_index != NULL);
  DCHECK(neighbor_voxel_index != NULL);

  *neighbor_block_index = block_index;
  *neighbor_voxel_index = voxel_index + direction;

  for (unsigned int i = 0; i < 3; ++i) {
    if ((*neighbor_voxel_index)(i) < 0) {
      (*neighbor_block_index)(i)--;
      (*neighbor_voxel_index)(i) += esdf_voxels_per_side_;
    } else if ((*neighbor_voxel_index)(i) >=
               static_cast<IndexElement>(esdf_voxels_per_side_)) {
      (*neighbor_block_index)(i)++;
      (*neighbor_voxel_index)(i) -= esdf_voxels_per_side_;
    }
  }
}

bool SkeletonGenerator::followEdge(const BlockIndex& start_block_index,
                                   const VoxelIndex& start_voxel_index,
                                   const Eigen::Vector3i& direction_from_vertex,
                                   int64_t* connected_vertex_id,
                                   float* min_distance, float* max_distance) {
  BlockIndex block_index = start_block_index;
  VoxelIndex voxel_index = start_voxel_index;

  Block<SkeletonVoxel>::Ptr block_ptr =
      skeleton_layer_->getBlockPtrByIndex(start_block_index);
  CHECK(block_ptr);
  SkeletonVoxel& voxel = block_ptr->getVoxelByVoxelIndex(start_voxel_index);

  *min_distance = voxel.distance;
  *max_distance = voxel.distance;
  Eigen::Vector3i last_direction = direction_from_vertex;

  bool vertex_found = false;
  bool still_got_neighbors = true;
  const int kMaxFollows = 300;
  int j = 0;
  // For now only ever search the first edge for some reason.
  while (vertex_found == false && still_got_neighbors == true &&
         j < kMaxFollows) {
    AlignedVector<VoxelKey> neighbors;
    AlignedVector<float> distances;
    AlignedVector<Eigen::Vector3i> directions;

    getNeighborsAndDistances(block_index, voxel_index, 26, &neighbors,
                             &distances, &directions);
    still_got_neighbors = false;

    // Choose the best candidate neighbor with the biggest dot product to the
    // parent vertex direction (attempt to follow straight line away from
    // vertex).
    size_t best_neighbor = 0;
    float best_dot_prod = -2.0;

    for (size_t i = 0; i < neighbors.size(); ++i) {
      if (directions[i] == -last_direction) {
        continue;
      }

      BlockIndex neighbor_block_index = neighbors[i].first;
      VoxelIndex neighbor_voxel_index = neighbors[i].second;
      Block<SkeletonVoxel>::Ptr neighbor_block;
      if (neighbor_block_index == block_index) {
        neighbor_block = block_ptr;
      } else {
        neighbor_block =
            skeleton_layer_->getBlockPtrByIndex(neighbor_block_index);
      }
      if (!neighbor_block) {
        continue;
      }
      SkeletonVoxel& neighbor_voxel =
          neighbor_block->getVoxelByVoxelIndex(neighbor_voxel_index);
      if (neighbor_voxel.is_vertex) {
        // UUUUHHHH how do I find which ID this vertex belongs to...
        // Just have its point :(
        *connected_vertex_id = neighbor_voxel.vertex_id;
        return true;
      }
      if (neighbor_voxel.is_edge) {
        still_got_neighbors = true;
        float dot_prod = direction_from_vertex.cast<float>().normalized().dot(
            directions[i].cast<float>().normalized());
        if (dot_prod > best_dot_prod) {
          best_neighbor = i;
          best_dot_prod = dot_prod;
        }
      }
    }

    if (still_got_neighbors) {
      // Get the best one out AGAIN...
      BlockIndex neighbor_block_index = neighbors[best_neighbor].first;
      VoxelIndex neighbor_voxel_index = neighbors[best_neighbor].second;
      Block<SkeletonVoxel>::Ptr neighbor_block;
      if (neighbor_block_index == block_index) {
        neighbor_block = block_ptr;
      } else {
        neighbor_block =
            skeleton_layer_->getBlockPtrByIndex(neighbor_block_index);
      }
      SkeletonVoxel& neighbor_voxel =
          neighbor_block->getVoxelByVoxelIndex(neighbor_voxel_index);

      if (neighbor_voxel.distance < *min_distance) {
        *min_distance = neighbor_voxel.distance;
      }
      if (neighbor_voxel.distance > *max_distance) {
        *max_distance = neighbor_voxel.distance;
      }

      block_index = neighbor_block_index;
      voxel_index = neighbor_voxel_index;
      block_ptr = neighbor_block;
      last_direction = directions[best_neighbor];
      j++;
    }
  }

  return false;
}

// Checks whether a point is simple, i.e., if its removal would not affect
// the connectivity of its neighbors.
// Uses a SIMILAR numbering to our bitset definitions, except without counting
// 13 (the middle point). So it's a bitset of 26 rather than 27.
// Adapted from Skeletonize3D in ImageJ.
// http://imagejdocu.tudor.lu/doku.php?id=plugin:morphology:skeletonize3d:start
bool SkeletonGenerator::isSimplePoint(const std::bitset<27>& neighbors) const {
  // copy neighbors for labeling
  std::vector<int> cube(26, 0);
  int i;
  for (i = 0; i < 13; i++) {  // i =  0..12 -> cube[0..12]
    cube[i] = neighbors[i];
  }
  // i != 13 : ignore center pixel when counting (see [Lee94])
  for (i = 14; i < 27; i++) {  // i = 14..26 -> cube[13..25]
    cube[i - 1] = neighbors[i];
  }
  // set initial label
  int label = 2;
  // for all points in the neighborhood
  for (i = 0; i < 26; i++) {
    if (cube[i] == 1)  // voxel has not been labeled yet
    {
      // start recursion with any octant that contains the point i
      switch (i) {
        case 0:
        case 1:
        case 3:
        case 4:
        case 9:
        case 10:
        case 12:
          octreeLabeling(1, label, &cube);
          break;
        case 2:
        case 5:
        case 11:
        case 13:
          octreeLabeling(2, label, &cube);
          break;
        case 6:
        case 7:
        case 14:
        case 15:
          octreeLabeling(3, label, &cube);
          break;
        case 8:
        case 16:
          octreeLabeling(4, label, &cube);
          break;
        case 17:
        case 18:
        case 20:
        case 21:
          octreeLabeling(5, label, &cube);
          break;
        case 19:
        case 22:
          octreeLabeling(6, label, &cube);
          break;
        case 23:
        case 24:
          octreeLabeling(7, label, &cube);
          break;
        case 25:
          octreeLabeling(8, label, &cube);
          break;
      }
      label++;
      if (label - 2 >= 2) {
        return false;
      }
    }
  }
  return true;
}

// This is a recursive method that calculates the number of connected
// components in the 3D neighborhood after the center pixel would
// have been removed.
// From Skeletonize3D from ImageJ.
void SkeletonGenerator::octreeLabeling(int octant, int label,
                                       std::vector<int>* cube) const {
  // check if there are points in the octant with value 1
  if (octant == 1) {
    // set points in this octant to current label
    // and recursive labeling of adjacent octants
    if ((*cube)[0] == 1) {
      (*cube)[0] = label;
    }
    if ((*cube)[1] == 1) {
      (*cube)[1] = label;
      octreeLabeling(2, label, cube);
    }
    if ((*cube)[3] == 1) {
      (*cube)[3] = label;
      octreeLabeling(3, label, cube);
    }
    if ((*cube)[4] == 1) {
      (*cube)[4] = label;
      octreeLabeling(2, label, cube);
      octreeLabeling(3, label, cube);
      octreeLabeling(4, label, cube);
    }
    if ((*cube)[9] == 1) {
      (*cube)[9] = label;
      octreeLabeling(5, label, cube);
    }
    if ((*cube)[10] == 1) {
      (*cube)[10] = label;
      octreeLabeling(2, label, cube);
      octreeLabeling(5, label, cube);
      octreeLabeling(6, label, cube);
    }
    if ((*cube)[12] == 1) {
      (*cube)[12] = label;
      octreeLabeling(3, label, cube);
      octreeLabeling(5, label, cube);
      octreeLabeling(7, label, cube);
    }
  }
  if (octant == 2) {
    if ((*cube)[1] == 1) {
      (*cube)[1] = label;
      octreeLabeling(1, label, cube);
    }
    if ((*cube)[4] == 1) {
      (*cube)[4] = label;
      octreeLabeling(1, label, cube);
      octreeLabeling(3, label, cube);
      octreeLabeling(4, label, cube);
    }
    if ((*cube)[10] == 1) {
      (*cube)[10] = label;
      octreeLabeling(1, label, cube);
      octreeLabeling(5, label, cube);
      octreeLabeling(6, label, cube);
    }
    if ((*cube)[2] == 1) {
      (*cube)[2] = label;
    }
    if ((*cube)[5] == 1) {
      (*cube)[5] = label;
      octreeLabeling(4, label, cube);
    }
    if ((*cube)[11] == 1) {
      (*cube)[11] = label;
      octreeLabeling(6, label, cube);
    }
    if ((*cube)[13] == 1) {
      (*cube)[13] = label;
      octreeLabeling(4, label, cube);
      octreeLabeling(6, label, cube);
      octreeLabeling(8, label, cube);
    }
  }
  if (octant == 3) {
    if ((*cube)[3] == 1) {
      (*cube)[3] = label;
      octreeLabeling(1, label, cube);
    }
    if ((*cube)[4] == 1) {
      (*cube)[4] = label;
      octreeLabeling(1, label, cube);
      octreeLabeling(2, label, cube);
      octreeLabeling(4, label, cube);
    }
    if ((*cube)[12] == 1) {
      (*cube)[12] = label;
      octreeLabeling(1, label, cube);
      octreeLabeling(5, label, cube);
      octreeLabeling(7, label, cube);
    }
    if ((*cube)[6] == 1) {
      (*cube)[6] = label;
    }
    if ((*cube)[7] == 1) {
      (*cube)[7] = label;
      octreeLabeling(4, label, cube);
    }
    if ((*cube)[14] == 1) {
      (*cube)[14] = label;
      octreeLabeling(7, label, cube);
    }
    if ((*cube)[15] == 1) {
      (*cube)[15] = label;
      octreeLabeling(4, label, cube);
      octreeLabeling(7, label, cube);
      octreeLabeling(8, label, cube);
    }
  }
  if (octant == 4) {
    if ((*cube)[4] == 1) {
      (*cube)[4] = label;
      octreeLabeling(1, label, cube);
      octreeLabeling(2, label, cube);
      octreeLabeling(3, label, cube);
    }
    if ((*cube)[5] == 1) {
      (*cube)[5] = label;
      octreeLabeling(2, label, cube);
    }
    if ((*cube)[13] == 1) {
      (*cube)[13] = label;
      octreeLabeling(2, label, cube);
      octreeLabeling(6, label, cube);
      octreeLabeling(8, label, cube);
    }
    if ((*cube)[7] == 1) {
      (*cube)[7] = label;
      octreeLabeling(3, label, cube);
    }
    if ((*cube)[15] == 1) {
      (*cube)[15] = label;
      octreeLabeling(3, label, cube);
      octreeLabeling(7, label, cube);
      octreeLabeling(8, label, cube);
    }
    if ((*cube)[8] == 1) {
      (*cube)[8] = label;
    }
    if ((*cube)[16] == 1) {
      (*cube)[16] = label;
      octreeLabeling(8, label, cube);
    }
  }
  if (octant == 5) {
    if ((*cube)[9] == 1) {
      (*cube)[9] = label;
      octreeLabeling(1, label, cube);
    }
    if ((*cube)[10] == 1) {
      (*cube)[10] = label;
      octreeLabeling(1, label, cube);
      octreeLabeling(2, label, cube);
      octreeLabeling(6, label, cube);
    }
    if ((*cube)[12] == 1) {
      (*cube)[12] = label;
      octreeLabeling(1, label, cube);
      octreeLabeling(3, label, cube);
      octreeLabeling(7, label, cube);
    }
    if ((*cube)[17] == 1) {
      (*cube)[17] = label;
    }
    if ((*cube)[18] == 1) {
      (*cube)[18] = label;
      octreeLabeling(6, label, cube);
    }
    if ((*cube)[20] == 1) {
      (*cube)[20] = label;
      octreeLabeling(7, label, cube);
    }
    if ((*cube)[21] == 1) {
      (*cube)[21] = label;
      octreeLabeling(6, label, cube);
      octreeLabeling(7, label, cube);
      octreeLabeling(8, label, cube);
    }
  }
  if (octant == 6) {
    if ((*cube)[10] == 1) {
      (*cube)[10] = label;
      octreeLabeling(1, label, cube);
      octreeLabeling(2, label, cube);
      octreeLabeling(5, label, cube);
    }
    if ((*cube)[11] == 1) {
      (*cube)[11] = label;
      octreeLabeling(2, label, cube);
    }
    if ((*cube)[13] == 1) {
      (*cube)[13] = label;
      octreeLabeling(2, label, cube);
      octreeLabeling(4, label, cube);
      octreeLabeling(8, label, cube);
    }
    if ((*cube)[18] == 1) {
      (*cube)[18] = label;
      octreeLabeling(5, label, cube);
    }
    if ((*cube)[21] == 1) {
      (*cube)[21] = label;
      octreeLabeling(5, label, cube);
      octreeLabeling(7, label, cube);
      octreeLabeling(8, label, cube);
    }
    if ((*cube)[19] == 1) {
      (*cube)[19] = label;
    }
    if ((*cube)[22] == 1) {
      (*cube)[22] = label;
      octreeLabeling(8, label, cube);
    }
  }
  if (octant == 7) {
    if ((*cube)[12] == 1) {
      (*cube)[12] = label;
      octreeLabeling(1, label, cube);
      octreeLabeling(3, label, cube);
      octreeLabeling(5, label, cube);
    }
    if ((*cube)[14] == 1) {
      (*cube)[14] = label;
      octreeLabeling(3, label, cube);
    }
    if ((*cube)[15] == 1) {
      (*cube)[15] = label;
      octreeLabeling(3, label, cube);
      octreeLabeling(4, label, cube);
      octreeLabeling(8, label, cube);
    }
    if ((*cube)[20] == 1) {
      (*cube)[20] = label;
      octreeLabeling(5, label, cube);
    }
    if ((*cube)[21] == 1) {
      (*cube)[21] = label;
      octreeLabeling(5, label, cube);
      octreeLabeling(6, label, cube);
      octreeLabeling(8, label, cube);
    }
    if ((*cube)[23] == 1) {
      (*cube)[23] = label;
    }
    if ((*cube)[24] == 1) {
      (*cube)[24] = label;
      octreeLabeling(8, label, cube);
    }
  }
  if (octant == 8) {
    if ((*cube)[13] == 1) {
      (*cube)[13] = label;
      octreeLabeling(2, label, cube);
      octreeLabeling(4, label, cube);
      octreeLabeling(6, label, cube);
    }
    if ((*cube)[15] == 1) {
      (*cube)[15] = label;
      octreeLabeling(3, label, cube);
      octreeLabeling(4, label, cube);
      octreeLabeling(7, label, cube);
    }
    if ((*cube)[16] == 1) {
      (*cube)[16] = label;
      octreeLabeling(4, label, cube);
    }
    if ((*cube)[21] == 1) {
      (*cube)[21] = label;
      octreeLabeling(5, label, cube);
      octreeLabeling(6, label, cube);
      octreeLabeling(7, label, cube);
    }
    if ((*cube)[22] == 1) {
      (*cube)[22] = label;
      octreeLabeling(6, label, cube);
    }
    if ((*cube)[24] == 1) {
      (*cube)[24] = label;
      octreeLabeling(7, label, cube);
    }
    if ((*cube)[25] == 1) {
      (*cube)[25] = label;
    }
  }
}

bool SkeletonGenerator::isEndPoint(const std::bitset<27>& neighbors) const {
  if (neighbors.count() == 1) {
    return true;
  }
  // Check first that we have at most 1 6-connected component.
  std::bitset<27> neighbor_mask_6 =
      corner_template_matcher_.get6ConnNeighborMask();
  if ((neighbor_mask_6 & neighbors).count() > 1) {
    return false;
  }
  if (corner_template_matcher_.fitsTemplates(neighbors)) {
    return true;
  }
  return false;
}

void SkeletonGenerator::pruneDiagramVertices() {
  // Ok, first set up a kdtree/nanoflann instance using the skeleton point
  // wrapper.
  // We want it dynamic because we are gonna be dropping hella vertices.
  // Or do we need to?
  const int kDim = 3;
  const int kMaxLeaf = 10;

  // Create the adapter.
  SkeletonPointVectorAdapter adapter(skeleton_.getVertexPoints());

  // construct a kd-tree index:
  typedef nanoflann::KDTreeSingleIndexDynamicAdaptor<
      nanoflann::L2_Simple_Adaptor<FloatingPoint, SkeletonPointVectorAdapter>,
      SkeletonPointVectorAdapter, kDim> SkeletonKdTree;

  SkeletonKdTree kd_tree(kDim, adapter,
                         nanoflann::KDTreeSingleIndexAdaptorParams(kMaxLeaf));

  // This would be buildIndex if we were doing the non-dynamic version...
  kd_tree.addPoints(0, adapter.kdtree_get_point_count());


}

}  // namespace voxblox