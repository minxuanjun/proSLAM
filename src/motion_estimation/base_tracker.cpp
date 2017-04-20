#include "base_tracker.h"

namespace proslam {
  using namespace srrg_core;

  BaseTracker::BaseTracker(): _camera_rows(0), _camera_cols(0), _context(0), _has_odometry(false) {
    _camera_left=0;
    _pose_optimizer=0;
    _framepoint_generator=0;
    _intensity_image_left=0;
  }

  void BaseTracker::setup() {
    assert(_camera_left);
    assert(_pose_optimizer);
    _camera_rows = _camera_left->imageRows();
    _camera_cols = _camera_left->imageCols();
    _motion_previous_to_current_robot.setIdentity();
    _pose_optimizer->setMaximumDepthNearMeters(_framepoint_generator->maximumDepthNearMeters());
    _pose_optimizer->setMaximumDepthFarMeters(_framepoint_generator->maximumDepthFarMeters());

    //ds clear buffers
    _lost_points.clear();
    _projected_image_coordinates_left.clear();
  }

  //ds dynamic cleanup
  BaseTracker::~BaseTracker() {
    std::cerr << "BaseTracker::BaseTracker|destroying" << std::endl;
    
    //ds clear buffers
    _lost_points.clear();
    _projected_image_coordinates_left.clear();

    //ds free dynamics
    delete _framepoint_generator;
    delete _pose_optimizer;
    std::cerr << "BaseTracker::BaseTracker|destroyed" << std::endl;
  }

  //ds creates a new Frame for the given images, retrieves the correspondences relative to the previous Frame, optimizes the current frame pose and updates landmarks
  void BaseTracker::compute() {
    assert(_camera_left);
    assert(_context);
    assert(_intensity_image_left);
    
    //ds reset point configurations
    _number_of_tracked_points        = 0;
    _number_of_lost_points           = 0;
    _number_of_lost_points_recovered = 0;
    for (Landmark* landmark: _context->currentlyTrackedLandmarks()) {
      landmark->setIsCurrentlyTracked(false);
    }
    _context->currentlyTrackedLandmarks().clear();

    //gg if we have an odometry we use it as initial guess
    if (_has_odometry) {
      if (! _context->currentFrame()){
        _previous_odometry = _odometry;
      }
      TransformMatrix3D odom_delta = _previous_odometry.inverse()*_odometry;
      _motion_previous_to_current_robot = odom_delta;
      _previous_odometry = _odometry;
    }

    //ds retrieve estimate by applying the constant velocity motion model
    if (_context->currentFrame()) {
      _context->setRobotToWorld(_context->robotToWorld()*_motion_previous_to_current_robot);
    }

    //ds create new frame
    Frame* current_frame = _makeFrame();
 
    //ds compute full sensory prior for the current frame
    _framepoint_generator->compute(current_frame);
    _number_of_potential_points = _framepoint_generator->numberOfAvailablePoints();

    //ds if available - attempt to track the points from the previous frame
    if (current_frame->previous()) {
      CHRONOMETER_START(tracking);
      _trackFramepoints(current_frame->previous(), current_frame);
      CHRONOMETER_STOP(tracking);
    }

    //ds check tracker status
    switch(_status) {

      //ds track lost - localizing
      case Frame::Localizing: {
        std::cerr << "BaseTracker::addImage|STATE: LOCALIZING" << std::endl;

        //ds if we have a previous frame
        if (current_frame->previous()) {

          //ds solve pose on frame points only
          CHRONOMETER_START(pose_optimization);
          _pose_optimizer->init(current_frame, current_frame->robotToWorld());
          _pose_optimizer->setWeightFramepoint(1);
          _pose_optimizer->converge();
          CHRONOMETER_STOP(pose_optimization);

          //ds if the pose computation is acceptable
          if (_pose_optimizer->numberOfInliers() > 2*_minimum_number_of_landmarks_to_track) {

            //ds compute resulting motion
            _motion_previous_to_current_robot = current_frame->previous()->worldToRobot()*_pose_optimizer->robotToWorld();
            const real delta_angular          = WorldMap::toOrientationRodrigues(_motion_previous_to_current_robot.linear()).norm();
            const real delta_translational    = _motion_previous_to_current_robot.translation().norm();

              //ds if the posit result is significant enough
            if (delta_angular > 0.001 || delta_translational > 0.01) {

              //ds update tracker
              current_frame->setRobotToWorld(_pose_optimizer->robotToWorld());
              std::cerr << "BaseTracker::addImage|WARNING: using posit on frame points (experimental) inliers: " << _pose_optimizer->numberOfInliers()
                        << " outliers: " << _pose_optimizer->numberOfOutliers() << " average error: " << _pose_optimizer->totalError()/_pose_optimizer->numberOfInliers() <<  std::endl;
            } else {

              //ds keep previous solution
              current_frame->setRobotToWorld(current_frame->previous()->robotToWorld());
              _motion_previous_to_current_robot = TransformMatrix3D::Identity();
            }

            //ds update context position
            _context->setRobotToWorld(current_frame->robotToWorld());
          }
        }

        //ds check if we can switch the state
        const Count number_of_good_points = current_frame->countPoints(current_frame->minimumTrackLengthForLandmarkCreation());
        if (number_of_good_points > _minimum_number_of_landmarks_to_track) {

          //ds trigger landmark creation and framepoint update
          _updateLandmarks(_context, current_frame);
          _status_previous = _status;
          _status = Frame::Tracking;
        } else {

          //ds just trigger framepoint updates
          current_frame->updatePoints();
        }
        break;
      }

      //ds on the track
      case Frame::Tracking: {

        //ds compute far to close landmark ratio TODO simplify or get better logic: currently the idea is to give more weight to framepoints in case we have almost only far landmarks
        const real weight_framepoint = 1-(_number_of_tracked_landmarks_far+7*_number_of_tracked_landmarks_close)/static_cast<real>(_number_of_tracked_points);
        assert(weight_framepoint <= 1);

        //ds call pose solver
        CHRONOMETER_START(pose_optimization)
        _pose_optimizer->init(current_frame, current_frame->robotToWorld());
        _pose_optimizer->setWeightFramepoint(std::max(weight_framepoint, static_cast<real>(0.1)));
        _pose_optimizer->converge();
        CHRONOMETER_STOP(pose_optimization)

        //ds solver deltas
        const Count& number_of_inliers = _pose_optimizer->numberOfInliers();
        // _motion_previous_to_current     = _pose_optimizer->robotToWorld()*current_frame->previous()->robotToWorld().inverse();

        _motion_previous_to_current_robot = current_frame->previous()->worldToRobot()*_pose_optimizer->robotToWorld();
        const real delta_angular          = WorldMap::toOrientationRodrigues(_motion_previous_to_current_robot.linear()).norm();
        const real delta_translational    = _motion_previous_to_current_robot.translation().norm();

        //ds if we don't have enough inliers - trigger fallback posit on last position
        if (number_of_inliers < _minimum_number_of_landmarks_to_track) {

          //ds reset state - also purging points to fully reinitialize the tracking
          std::cerr << "LOST TRACK due to invalid position optimization" << std::endl;
          _status_previous = Frame::Localizing;
          _status          = Frame::Localizing;
          current_frame->setStatus(_status);
          current_frame->releasePoints();
          _framepoint_generator->clearFramepointsInImage();
          _context->currentlyTrackedLandmarks().clear();

          //ds keep previous solution
          current_frame->setRobotToWorld(current_frame->previous()->robotToWorld());
          _motion_previous_to_current_robot = TransformMatrix3D::Identity();
          _context->setRobotToWorld(current_frame->robotToWorld());
          return;
        }

        //ds if the posit result is significant enough
        if (delta_angular > 0.001 || delta_translational > 0.01) {

          //ds update robot pose with posit result
          current_frame->setRobotToWorld(_pose_optimizer->robotToWorld());
        } else {

          //ds keep previous solution
          current_frame->setRobotToWorld(current_frame->previous()->robotToWorld());
          _motion_previous_to_current_robot = TransformMatrix3D::Identity();
        }

        //ds visualization only (we need to clear and push_back in order to not crash the gui since its decoupled - otherwise we could use resize)
        _context->currentlyTrackedLandmarks().reserve(_number_of_tracked_landmarks_far+_number_of_tracked_landmarks_close);

        //ds prune current frame points
        _pruneFramepoints(current_frame);
        assert(_context->currentlyTrackedLandmarks().size() <= _number_of_tracked_landmarks_far+_number_of_tracked_landmarks_close);
        assert(_number_of_tracked_points >= number_of_inliers);

        //ds recover lost points based on updated pose
        CHRONOMETER_START(point_recovery)
        _recoverPoints(current_frame);
        CHRONOMETER_STOP(point_recovery)

        //ds update tracks
        _context->setRobotToWorld(current_frame->robotToWorld());
        CHRONOMETER_START(landmark_optimization)
        _updateLandmarks(_context, current_frame);
        CHRONOMETER_STOP(landmark_optimization)
        _status_previous = _status;
        _status          = Frame::Tracking;
        break;
      }

      default: {
        assert(false);
        break;
      }
    }

    //ds add new framepoints
    assert(current_frame != 0);
    CHRONOMETER_START(track_creation)
    _addNewFramepoints(current_frame);
    CHRONOMETER_STOP(track_creation)
    current_frame->setStatus(_status);

    //ds done
    _total_number_of_tracked_points += _number_of_tracked_points;
  }

  //ds retrieves framepoint correspondences between previous and current frame
  void BaseTracker::_trackFramepoints(Frame* previous_frame_, Frame* current_frame_) {
    assert(previous_frame_ != 0);
    assert(current_frame_ != 0);

    //ds control variables
    current_frame_->points().resize(_number_of_potential_points);
    _number_of_tracked_points          = 0;
    _number_of_tracked_landmarks_close = 0;
    _number_of_tracked_landmarks_far   = 0;
    _number_of_lost_points             = 0;

    //ds retrieve point predictions on current image plane
    _getImageCoordinates(_projected_image_coordinates_left, previous_frame_, current_frame_);

    //ds prepare lost buffer
    _lost_points.resize(previous_frame_->points().size());

    //ds check state
    if (_status_previous == Frame::Localizing) {

      //ds for localization mode we have a more relaxed tracking condition
      _pixel_distance_tracking_threshold = _pixel_distance_tracking_threshold_maximum;
    } else {

      //ds narrow search limit closer to projection when we're in tracking mode
      _pixel_distance_tracking_threshold = _pixel_distance_tracking_threshold_minimum;
    }
    const real _maximum_matching_distance_tracking_point  = _framepoint_generator->matchingDistanceTrackingThreshold();
    const real _maximum_matching_distance_tracking_region = _framepoint_generator->matchingDistanceTrackingThreshold();

    //ds loop over all past points
    for (Index index_point_previous = 0; index_point_previous < previous_frame_->points().size(); ++index_point_previous) {

      //ds compute current projection points
      FramePoint* previous_point = previous_frame_->points()[index_point_previous];
      const ImageCoordinates& projection_left(_projected_image_coordinates_left[index_point_previous]);

      //ds prior grid location
      const int32_t row_projection = std::round(projection_left.y());
      const int32_t col_projection = std::round(projection_left.x());
      const int32_t row_previous   = std::round(previous_point->imageCoordinatesLeft().y());
      const int32_t col_previous   = std::round(previous_point->imageCoordinatesLeft().x());

      //ds exhaustive search
      int32_t pixel_distance_best = _pixel_distance_tracking_threshold;
      int32_t row_best = -1;
      int32_t col_best = -1;

      //ds ------------------------------------------- STAGE 1: POINT VICINITY TRACKING
      //ds compute borders
      const int32_t row_start_point = std::max(row_projection-_range_point_tracking, static_cast<int32_t>(0));
      const int32_t row_end_point   = std::min(row_projection+_range_point_tracking, static_cast<int32_t>(_framepoint_generator->numberOfRowsImage()));
      const int32_t col_start_point = std::max(col_projection-_range_point_tracking, static_cast<int32_t>(0));
      const int32_t col_end_point   = std::min(col_projection+_range_point_tracking, static_cast<int32_t>(_framepoint_generator->numberOfColsImage()));

      //ds locate best match
      for (int32_t row_point = row_start_point; row_point < row_end_point; ++row_point) {
        for (int32_t col_point = col_start_point; col_point < col_end_point; ++col_point) {
          if (_framepoint_generator->framepointsInImage()[row_point][col_point]) {
            const int32_t pixel_distance = std::fabs(row_projection-row_point)+std::fabs(col_projection-col_point);
            const real matching_distance = cv::norm(previous_point->descriptorLeft(),
                                                    _framepoint_generator->framepointsInImage()[row_point][col_point]->descriptorLeft(),
                                                    DESCRIPTOR_NORM);

            if (pixel_distance < pixel_distance_best && matching_distance < _maximum_matching_distance_tracking_point) {
              pixel_distance_best = pixel_distance;
              row_best = row_point;
              col_best = col_point;
            }
          }
        }
      }

      //ds if we found a match
      if (pixel_distance_best < _pixel_distance_tracking_threshold) {

        //ds check if track is consistent
        if ((row_best-row_previous)*(row_best-row_previous)+(col_best-col_previous)*(col_best-col_previous) < _maximum_flow_pixels_squared) {

          //ds allocate a new point connected to the previous one
          FramePoint* current_point = _framepoint_generator->framepointsInImage()[row_best][col_best];
          current_point->setPrevious(previous_point);

          //ds set the point to the control structure
          current_frame_->points()[_number_of_tracked_points] = current_point;
          if (current_point->landmark()) {
            if (current_point->isNear()) {
              ++_number_of_tracked_landmarks_close;
            } else {
              ++_number_of_tracked_landmarks_far;
            }
          }

          const cv::Point2f point_previous(previous_point->imageCoordinatesLeft().x(), previous_point->imageCoordinatesLeft().y());
          const cv::Point2f point_current(current_point->imageCoordinatesLeft().x(), current_point->imageCoordinatesLeft().y());
          ++_number_of_tracked_points;

          //ds disable further matching and reduce search time
          _framepoint_generator->framepointsInImage()[row_best][col_best] = 0;
          continue;
        }
      }

      //ds ------------------------------------------- STAGE 2: REGIONAL TRACKING
      pixel_distance_best = _pixel_distance_tracking_threshold;

      //ds compute borders
      const int32_t row_start_region = std::max(row_projection-_pixel_distance_tracking_threshold, static_cast<int32_t>(0));
      const int32_t row_end_region   = std::min(row_projection+_pixel_distance_tracking_threshold, static_cast<int32_t>(_framepoint_generator->numberOfRowsImage()));
      const int32_t col_start_region = std::max(col_projection-_pixel_distance_tracking_threshold, static_cast<int32_t>(0));
      const int32_t col_end_region   = std::min(col_projection+_pixel_distance_tracking_threshold, static_cast<int32_t>(_framepoint_generator->numberOfColsImage()));

      //ds locate best match
      for (int32_t row_region = row_start_region; row_region < row_end_region; ++row_region) {
        for (int32_t col_region = col_start_region; col_region < col_end_region; ++col_region) {
          if (_framepoint_generator->framepointsInImage()[row_region][col_region]) {

            //ds if area has not been already evaluated in previous stage
            if (row_region < row_start_point||
                row_region >= row_end_point ||
                col_region < col_start_point||
                col_region >= col_end_point ) {

              const int32_t pixel_distance = std::fabs(row_projection-row_region)+std::fabs(col_projection-col_region);
              const real matching_distance = cv::norm(previous_point->descriptorLeft(),
                                                      _framepoint_generator->framepointsInImage()[row_region][col_region]->descriptorLeft(),
                                                      DESCRIPTOR_NORM);

              if (pixel_distance < pixel_distance_best && matching_distance < _maximum_matching_distance_tracking_region) {
                pixel_distance_best = pixel_distance;
                row_best = row_region;
                col_best = col_region;
              }
            }
          }
        }
      }

      //ds if we found a match
      if (pixel_distance_best < _pixel_distance_tracking_threshold) {

        //ds check if track is consistent
        if ((row_best-row_previous)*(row_best-row_previous)+(col_best-col_previous)*(col_best-col_previous) < _maximum_flow_pixels_squared) {

          //ds allocate a new point connected to the previous one
          FramePoint* current_point = _framepoint_generator->framepointsInImage()[row_best][col_best];
          current_point->setPrevious(previous_point);

          //ds set the point to the control structure
          current_frame_->points()[_number_of_tracked_points] = current_point;
          if (current_point->landmark()) {
            if (current_point->isNear()) {
              ++_number_of_tracked_landmarks_close;
            } else {
              ++_number_of_tracked_landmarks_far;
            }
          }

          const cv::Point2f point_previous(previous_point->imageCoordinatesLeft().x(), previous_point->imageCoordinatesLeft().y());
          const cv::Point2f point_current(current_point->imageCoordinatesLeft().x(), current_point->imageCoordinatesLeft().y());
          ++_number_of_tracked_points;

          //ds disable further matching and reduce search time
          _framepoint_generator->framepointsInImage()[row_best][col_best] = 0;
          continue;
        }
      }

      //ds no match found
      if (previous_point->landmark()) {
        _lost_points[_number_of_lost_points] = previous_point;
        ++_number_of_lost_points;
      }
    }
    current_frame_->points().resize(_number_of_tracked_points);
    _lost_points.resize(_number_of_lost_points);

    //    //ds info
    //    std::cerr << "BaseTracker::trackFeatures|tracks: " << _number_of_tracked_points << "/" << previous_frame_->points().size()
    //              << " landmarks close: " << _number_of_tracked_landmarks_close
    //              << " landmarks far: " << _number_of_tracked_landmarks_far
    //              << std::endl;
    _total_number_of_landmarks_close += _number_of_tracked_landmarks_close;
    _total_number_of_landmarks_far   += _number_of_tracked_landmarks_far;
  }

  //ds adds new framepoints to the provided frame (picked from the pool of the _framepoint_generator)
  void BaseTracker::_addNewFramepoints(Frame* frame_) {

    //ds make space for all remaining points
    frame_->points().resize(_number_of_potential_points+_number_of_lost_points_recovered);

    //ds buffer current pose
    const TransformMatrix3D& frame_to_world = frame_->robotToWorld();

    //ds check triangulation map for unmatched points
    Index index_point_new = _number_of_tracked_points;
    for (uint32_t row = 0; row < _framepoint_generator->numberOfRowsImage(); ++row) {
      for (uint32_t col = 0; col < _framepoint_generator->numberOfColsImage(); ++col) {
        if (_framepoint_generator->framepointsInImage()[row][col]) {

          //ds assign the new point
          frame_->points()[index_point_new] = _framepoint_generator->framepointsInImage()[row][col];

          //ds update framepoint world position using the current pose estimate
          frame_->points()[index_point_new]->setWorldCoordinates(frame_to_world*frame_->points()[index_point_new]->robotCoordinates());

          //ds free point from input grid
          _framepoint_generator->framepointsInImage()[row][col] = 0;
          ++index_point_new;
        }
      }
    }
    frame_->points().resize(index_point_new);
    //    std::cerr << "BaseTracker::extractFeatures|new points: " << index_point_new-_number_of_tracked_points << std::endl;
  }

  //ds retrieves framepoint projections as image coordinates in a vector (at the same time removing points with invalid projections)
  void BaseTracker::_getImageCoordinates(std::vector<ImageCoordinates>& projected_image_coordinates_left_, Frame* previous_frame_, const Frame* current_frame_) const {
    assert(previous_frame_ != 0);
    assert(current_frame_ != 0);

    //ds preallocation
    projected_image_coordinates_left_.resize(previous_frame_->points().size());
    const TransformMatrix3D world_to_camera = current_frame_->cameraLeft()->robotToCamera()*current_frame_->worldToRobot();

    //ds compute predictions for all previous frame points
    Count number_of_visible_points = 0;
    for (FramePoint* previous_frame_point: previous_frame_->points()) {
      assert(previous_frame_point->imageCoordinatesLeft().x() >= 0);
      assert(previous_frame_point->imageCoordinatesLeft().x() <= _camera_cols);
      assert(previous_frame_point->imageCoordinatesLeft().y() >= 0);
      assert(previous_frame_point->imageCoordinatesLeft().y() <= _camera_rows);

      //ds get point into current camera - based on last track
      Vector3 point_in_camera;

      //ds if we have a valid landmark at hand
      if (previous_frame_point->landmark() && previous_frame_point->landmark()->areCoordinatesValidated()) {

        //ds get point in camera frame based on landmark coordinates
        point_in_camera = world_to_camera*previous_frame_point->landmark()->coordinates();
      } else {

        //ds reproject based on last track
        point_in_camera = world_to_camera*previous_frame_point->worldCoordinates();
      }

      //ds obtain point projection on camera image plane
      PointCoordinates point_in_image_left = current_frame_->cameraLeft()->cameraMatrix()*point_in_camera;

      //ds normalize point and update prediction based on landmark position: LEFT
      point_in_image_left /= point_in_image_left.z();

      //ds check for invalid projections
      if (point_in_image_left.x() < 0 || point_in_image_left.x() > _camera_cols||
          point_in_image_left.y() < 0 || point_in_image_left.y() > _camera_rows) {

        //ds out of FOV
        continue;
      }

      //ds update predictions
      projected_image_coordinates_left_[number_of_visible_points] = point_in_image_left;
      previous_frame_->points()[number_of_visible_points] = previous_frame_point;
      ++number_of_visible_points;
    }
    previous_frame_->points().resize(number_of_visible_points);
    projected_image_coordinates_left_.resize(number_of_visible_points);
  }

  //ds prunes invalid framespoints after pose optimization
  void BaseTracker::_pruneFramepoints(Frame* frame_) {

    //ds update current frame points
    _number_of_tracked_points = 0;
    for (Index index_point = 0; index_point < frame_->points().size(); index_point++) {
      assert(frame_->points()[index_point]->previous());

      //ds buffer current landmark
      Landmark* landmark = frame_->points()[index_point]->landmark();

      //ds points without landmarks are always kept
      if (landmark == 0) {
        frame_->points()[_number_of_tracked_points] = frame_->points()[index_point];
        ++_number_of_tracked_points;
      }

      //ds keep the point if it has been skipped (due to insufficient maturity or invalidity) or is an inlier
      else if (_pose_optimizer->errors()[index_point] == -1 || _pose_optimizer->inliers()[index_point]) {
        frame_->points()[_number_of_tracked_points] = frame_->points()[index_point];
        ++_number_of_tracked_points;
      }
    }
    frame_->points().resize(_number_of_tracked_points);
  }

  //ds updates existing or creates new landmarks for framepoints of the provided frame
  void BaseTracker::_updateLandmarks(WorldMap* context_, Frame* frame_) {

    //ds buffer current pose
    const TransformMatrix3D& frame_to_world = frame_->robotToWorld();

    //ds start landmark generation/update
    for (FramePoint* point: frame_->points()) {
      point->setWorldCoordinates(frame_to_world*point->robotCoordinates());

      //ds skip point if tracking and not mature enough to be a landmark - for localizing state this is skipped
      if (point->trackLength() < frame_->minimumTrackLengthForLandmarkCreation()) {
        continue;
      }

      //ds initial update setup
      Landmark* landmark = point->landmark();

      //ds if there's no landmark yet
      if (!landmark) {

        //ds create a landmark and associate it with the current framepoint
        landmark = context_->createLandmark(point);
        point->setLandmark(landmark);

	//        //ds include preceding measurements
	//        const FramePoint* previous = point->previous();
	//        while(previous) {
	//          landmark->update(previous);
	//          previous = previous->previous();
	//        }
      }
      assert(landmark != 0);

      //ds check if the current landmark position is near or far to the camera
      if (point->isNear()) {
        landmark->setIsNear(true);
      } else {
        landmark->setIsNear(false);
      }

      //ds update landmark position based on current point
      landmark->update(point);

      //ds add landmarks to currently visible ones
      landmark->setIsCurrentlyTracked(true);
      context_->currentlyTrackedLandmarks().push_back(landmark);
    }
  }
}
