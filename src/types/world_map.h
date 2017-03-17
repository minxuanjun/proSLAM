#pragma once
#include "local_map.h"

namespace proslam{
  class WorldMap{
  public: EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  //ds public object handling
  public:

    WorldMap();
    ~WorldMap();

  //ds interface
  public:

    void clear();

    FramePtrMap& frames() {return _frames;}
    const FramePtrMap& frames() const {return _frames;}
    LocalMapPointerVector& localMaps() {return _local_maps;}
    const LocalMapPointerVector& localMaps() const {return _local_maps;}
    Frame* createFrame(const TransformMatrix3D& robot_pose, const real& maximum_depth_close_);
    const bool createLocalMap();
    const FramePtrVector frameQueueForLocalMap() const {return _frame_queue_for_local_map;}

    LandmarkPtrMap& landmarks() {return _landmarks;}
    Landmark* createLandmark(const PointCoordinates& coordinates_in_world_ = PointCoordinates::Zero());

    Frame* rootFrame() {return _root_frame;}
    const Frame* currentFrame() const {return _current_frame;}
    const Frame* previousFrame() const {return _previous_frame;}
    Frame* currentFrame() {return _current_frame;}
    Frame* previousFrame() {return _previous_frame;}
    LocalMap* currentLocalMap() {return _current_local_map;}
    LocalMap* previousLocalMap() {assert(1 < _local_maps.size()); return *(_local_maps.end()-2);} //ds NASTY, price for readability
    void closeLocalMaps(LocalMap* query_, const LocalMap* reference_, const TransformMatrix3D& transform_query_to_reference_);

    void setRobotToWorldPrevious(const TransformMatrix3D& robot_pose_) {_last_good_robot_pose = robot_pose_;}
    const TransformMatrix3D robotToWorldPrevious() const {return _last_good_robot_pose;}
    const bool relocalized() const {return _relocalized;}

    void resetWindow();
    void purifyLandmarks();

    //ds dump trajectory to file (in KITTI benchmark format only for now)
    void writeTrajectory(const std::string& filename_ = "") const;

  //ds wrapped helpers TODO purge
  public:

    static const Eigen::Matrix<real, 3, 1> toOrientationRodrigues(const Eigen::Matrix<real, 3, 3>& p_matRotation)
    {
        //ds orientation
        cv::Mat vecOrientation;

        //ds fill the matrix
        cv::Rodrigues( toCVMatrix( p_matRotation ), vecOrientation );

        //ds return in Eigen format
        return fromCVVector< real, 3 >( vecOrientation );
    }

    static const cv::Mat_<real> toCVMatrix(const Eigen::Matrix<real, 3, 3>& p_matEigen)
    {
        //ds allocate cv vector
        cv::Mat_< real > matCV( 3, 3 );

        //ds fill the vector (column major)
        for( uint32_t u = 0; u < 3; ++u )
        {
            for( uint32_t v = 0; v < 3; ++v )
            {
                matCV.at< real >( u, v ) = p_matEigen( u, v );
            }
        }

        return matCV;
    }

    template<typename tType, uint32_t uRows> static const Eigen::Matrix<tType, uRows, 1> fromCVVector(const cv::Vec<tType, uRows>& p_vecCV)
    {
        //ds allocate eigen matrix
        Eigen::Matrix< tType, uRows, 1 > vecEigen;

        //ds fill the vector (column major)
        for( uint32_t u = 0; u < uRows; ++u )
        {
            vecEigen( u ) = p_vecCV( u );
        }

        return vecEigen;
    }

  protected:

    Frame* _root_frame     = 0;
    Frame* _current_frame  = 0;
    Frame* _previous_frame = 0;
    LandmarkPtrMap _landmarks;
    FramePtrMap _frames;

    //ds localization
    TransformMatrix3D _last_good_robot_pose = TransformMatrix3D::Identity();
    bool _relocalized = false;

    //ds current frame window buffer for local map generation
    real _distance_traveled_window = 0.0;
    real _degrees_rotated_window   = 0.0;

    //ds key frame generation properties
    const real _minimum_distance_traveled_for_local_map = 0.5; //ds local map generation based on translational movement
    const real _minimum_degrees_rotated_for_local_map   = 0.5; //ds local map generation based on rotational movement
    const Count _minimum_number_of_frames_for_local_map = 4;   //ds in case translational local map generation is triggered, this value enforces a reasonable trajectory granularity

    //ds local map control structures
    FramePtrVector _frame_queue_for_local_map;
    LocalMap* _current_local_map  = 0;
    LocalMapPointerVector _local_maps;
  };
}