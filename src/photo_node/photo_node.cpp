/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2009, Robert Bosch LLC.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Robert Bosch nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *********************************************************************/

#include <gphoto2_ros/photo_node.h>

PhotoNode::PhotoNode(std::string name_action_set_focus, std::string name_action_trigger) :
  camera_list_(),
  camera_(),
  image_(),
  as_set_focus(nh, name_action_set_focus, boost::bind(&PhotoNode::execute_set_focus_CB, this, _1), false),
  as_trigger(nh, name_action_trigger, boost::bind(&PhotoNode::execute_trigger_CB, this, _1), false)
{

  ros::NodeHandle nh_priv("~");

  //Camera filters
  nh_priv.getParam("owner", owner_);

  //Camera configs
  nh_priv.getParam("shutter_speed_mode", shutter_speed_mode_);
  nh_priv.getParam("aperture_mode", aperture_mode_);
  nh_priv.getParam("iso_mode", iso_mode_);

  ROS_WARN("photo_node %s : Opening camera with owner field: %s", owner_.c_str() , owner_.c_str() );
  ROS_INFO("photo_node %s : waiting for camera to be plugged or switched on", owner_.c_str());
  while (!camera_initialization(owner_) && ros::ok()){
    //int int_owner=std::stoi(owner_);
    ros::Duration(2.0).sleep();
  }
  ROS_INFO("photo_node %s : Got camera, starting",owner_.c_str());
  ROS_INFO("photo_node %s : configuring",owner_.c_str());
  camera_configs(aperture_mode_,shutter_speed_mode_, iso_mode_);


  // ***** Start Services *****
  set_config_srv_ = nh.advertiseService("set_config", &PhotoNode::setConfig, this);
  get_config_srv_ = nh.advertiseService("get_config", &PhotoNode::getConfig, this);
  unlock_camera_srv_ = nh.advertiseService("unlock_camera", &PhotoNode::unlockCamera, this);
  download_pictures_srv_ = nh.advertiseService("download_pictures", &PhotoNode::downloadPictures, this);
  get_picture_path_list_srv_ = nh.advertiseService("get_picture_path_list", &PhotoNode::getPicturePathList, this);
  reset_picture_path_list_srv_ = nh.advertiseService("reset_picture_path_list", &PhotoNode::resetPicturePathList, this);
  delete_pictures_srv_ = nh.advertiseService("delete_pictures", &PhotoNode::deletePictures, this);
  is_camera_ready_srv_ = nh.advertiseService("is_camera_ready", &PhotoNode::isCameraReady, this);
  get_config_client_ = nh.serviceClient<gphoto2_ros::GetConfig>("get_config");

  path_pub_ = nh.advertise<std_msgs::String>("canon/eos/picture_path", 10);

  as_set_focus.start();
  as_trigger.start();

  // ***** Loop to keep list of taken pictures updated
  picture_path_timer_ = nh.createTimer(ros::Duration(0.01), &PhotoNode::picturePathTimerCallback, this);
  //picture_path_timer_.stop();
  //reinit_camera_timer_ = nh.createTimer(ros::Duration(10), &PhotoNode::reinitCameraCallback, this);

}


PhotoNode::~PhotoNode()
{
  // shutdown camera
  exit_loop_ = true;
  camera_.photo_camera_close();
}


bool PhotoNode::camera_initialization(std::string desired_owner){
  //ROS_INFO( "photo_node: cam_init" );
  camera_list_=*(new photo_camera_list());
  camera_=*(new photo_camera());
  // create context
  private_context = camera_.photo_camera_create_context();

  // autodetect all cameras connected
  if( camera_list_.autodetect( private_context ) == false )
  {
    ROS_FATAL( "photo_node: Autodetection of cameras failed." );
    gp_context_unref( private_context );
    return false;
  }


  for (int i=0;i <gp_list_count(camera_list_.getCameraList()); i++) {

    if ( !camera_.photo_camera_open(&camera_list_,i)){
      ROS_WARN_STREAM("photo_node: Could not open camera n " << i);
      camera_.photo_camera_close();
      return false;
    }else {
      std::string port_info = camera_.get_port_info();
      if (isDeviceClaimed(port_info)){
       // ROS_WARN_STREAM("Device on " << port_info << " is already claimed");
        camera_.photo_camera_close();
      }else {

        char* value = new char[255];
        bool error_code = camera_.photo_camera_get_config("ownername", &value );
        ROS_WARN_STREAM("Owner : "<< value << " on port " << port_info << " / desired owner  : " << desired_owner);
        if( error_code && desired_owner==value)
        {

          current_port_info=camera_.get_port_info();
          ROS_WARN_STREAM("Initializing owner: "<< value << " on port " << current_port_info);
          is_camera_connected_=true;
          camera_list_.~photo_camera_list();
          ROS_INFO("Camera initialized");
          return true;
        }else {
          camera_.photo_camera_close();
          ros::Duration(1.0).sleep();
        }
        delete[] value;
      }

    }
  }

  gp_context_unref( private_context );
  camera_.photo_camera_close();
  camera_list_.~photo_camera_list();
  return false;
}



void PhotoNode::camera_configs(std::string aperture_mode, std::string shutter_speed_mode, std::string iso_mode){
  photo_mutex_.lock();
  ROS_INFO("Settings aperture/shutterspeed/iso: %s/%s/%s", aperture_mode.c_str(), shutter_speed_mode.c_str(), iso_mode.c_str());
  camera_.photo_camera_set_config( "aperture", aperture_mode_ );
  camera_.photo_camera_set_config( "shutterspeed", shutter_speed_mode_ );
  camera_.photo_camera_set_config( "iso", "1" );
  camera_.photo_camera_set_config( "iso", iso_mode_ );

  //Ensure record on SD and clock is sync
  camera_.photo_camera_set_config( "capturetarget", "1" );
  camera_.photo_camera_set_config( "syncdatetimeutc", "0" );
  is_camera_configured_=true;
  photo_mutex_.unlock();
}

void PhotoNode::execute_set_focus_CB(const gphoto2_ros::SetFocusGoalConstPtr &goal)
{
  if (is_camera_connected_ && is_camera_configured_){
    photo_mutex_.lock();
    bool error_code_focus_drive = camera_.photo_camera_set_config("autofocusdrive", "true");
    ros::Duration(1.5).sleep();
    bool error_code_cancel_focus = camera_.photo_camera_set_config("cancelautofocus", "true");
    photo_mutex_.unlock();
    if (error_code_focus_drive && error_code_cancel_focus){
      as_set_focus.setSucceeded();
    }else {
      as_set_focus.setAborted();
    }}
  else{
    ROS_WARN("Cameras are not ready, cannot set focus");
    as_set_focus.setAborted();
  }
}

void PhotoNode::execute_trigger_CB(const gphoto2_ros::TriggerGoalConstPtr &goal)
{
  ROS_INFO( "Triggering capture action" );
  if (is_camera_connected_ && is_camera_configured_){
    photo_mutex_.lock();
    trigger_count++;
    bool error_code_trigger = camera_.photo_camera_set_config("eosremoterelease", "5");
    photo_mutex_.unlock();
    if (error_code_trigger ){
      as_trigger.setSucceeded();
    }else {
      as_trigger.setAborted();
    }
  }
  else{
    ROS_WARN("Cameras are not ready, cannot trigger");
    as_set_focus.setAborted();
  }
}

bool PhotoNode::setConfig( gphoto2_ros::SetConfig::Request& req, gphoto2_ros::SetConfig::Response& resp )
{
  photo_mutex_.lock();
  bool error_code = camera_.photo_camera_set_config( req.param, req.value );
  photo_mutex_.unlock();
  return error_code;
}

bool PhotoNode::getConfig( gphoto2_ros::GetConfig::Request& req, gphoto2_ros::GetConfig::Response& resp )
{
  char* value = new char[255];
  photo_mutex_.lock();
  bool error_code = camera_.photo_camera_get_config( req.param, &value );
  if( error_code )
  {
    resp.value = value;
  }
  photo_mutex_.unlock();
  delete[] value;
  return error_code;
}


//Set the focus of the camera, the sleep in the middle of the function is necessary to give the camera time to focus properly
bool PhotoNode::setFocus(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& resp )
{
  photo_mutex_.lock();
  bool error_code_focus_drive = camera_.photo_camera_set_config("autofocusdrive", "true");
  ros::Duration(1.5).sleep();
  bool error_code_cancel_focus = camera_.photo_camera_set_config("cancelautofocus", "true");
  photo_mutex_.unlock();
  resp.success = true;
  return true;
}

//Take instantaneously a picture and save it in the memory card (be sure that capturetarget = 1 in the camera config)
bool PhotoNode::triggerCapture(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& resp) {
  std::cout << "Triggering capture service" << std::endl;
  photo_mutex_.lock();
  trigger_count++;
  bool error_code_focus_drive = camera_.photo_camera_set_config("eosremoterelease", "5");
  photo_mutex_.unlock();
  resp.success = true;
  return true;
}

//After taking pictures using triggerCapture the camera is locked in a state, this function unlock the camera and allow us to execute other actions
bool PhotoNode::unlockCamera(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& resp) {
  photo_mutex_.lock();
  bool error_code_focus_drive = camera_.photo_camera_set_config("eosremoterelease", "11");
  photo_mutex_.unlock();
  resp.success = true;
  return true;
}

//Downaload all the pictures in the contained in the req.camera_paths field (complete paths are necessary)
// into a folder precised in req.computer_path
bool PhotoNode::downloadPictures(gphoto2_ros::DownloadPictures::Request& req, gphoto2_ros::DownloadPictures::Response& resp) {

  while( trigger_count != picture_path_list.size()){
    ROS_WARN("Waiting to receive remaning picture path from camera");
    ROS_WARN_STREAM("Trigger count: " << trigger_count << " picture_path_list.size: " << picture_path_list.size());
    ros::Duration(0.5).sleep();
  }

  if (picture_path_list.size() != req.computer_paths.size()){
    ROS_WARN("requested paths list do not match camera path list: picture_path_list size: %d : computer_paths_size: %d", picture_path_list.size(), req.computer_paths.size());
    resp.success=false;
    return  true;
  }

  std::string delimiter = "/", on_camera_folder, on_camera_filename, on_computer_folder, on_computer_filename;

  ros::Time t_begin = ros::Time::now();
  ros::Duration mean_time;
  mean_time.fromSec(0);
  int c=0;
  //Pre treat all the data to get folder and file separated
  for(int i=0; i<picture_path_list.size() ; i++) {
    size_t cam_pos, compu_pos;

    cam_pos = picture_path_list[i].find_last_of('/');
    on_camera_folder = picture_path_list[i].substr(0, cam_pos+1);
    on_camera_filename = picture_path_list[i].substr(cam_pos+1);

    compu_pos = req.computer_paths[i].find_last_of('/');
    on_computer_folder = req.computer_paths[i].substr(0, compu_pos+1);
    on_computer_filename = req.computer_paths[i].substr(compu_pos+1);

    CameraFilePath path;
    std::strcpy(path.name, on_camera_filename.c_str());
    std::strcpy(path.folder, on_camera_folder.c_str());
    ros::Time t_lock = ros::Time::now();

    ROS_INFO_STREAM("Downloading " << path.folder << path.name << " on " << on_computer_folder << on_computer_filename);
    if(!std::experimental::filesystem::exists(req.computer_paths[i])) {
      photo_mutex_.lock();
      camera_.download_picture(path, on_computer_folder, on_computer_filename);
      photo_mutex_.unlock();
    } else {
      ROS_ERROR("The file already exist, picture won't be saved");
    }

    ros::Time t_unlock = ros::Time::now();
    mean_time += (t_unlock - t_lock);
    c++;
  }
  ros::Time t_end = ros::Time::now();
  std::cout << "Total duration : " << (t_end - t_begin).toSec() << " for " << c << " pictures" << std::endl;
  std::cout << "Mean lock time per pic : " << mean_time.toSec()/c << std::endl;
  resp.success = true;
  return true;
}

bool PhotoNode::getPicturePathList(gphoto2_ros::GetPicturePathList::Request& req, gphoto2_ros::GetPicturePathList::Response& resp) {
  resp.picture_path_list=picture_path_list;
  return true;
}

bool PhotoNode::resetPicturePathList(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& resp )
{
  picture_path_list.clear();
  trigger_count=0;
  resp.success = true;
  return true;
}

bool PhotoNode::isCameraReady(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& resp )
{
  resp.success = is_camera_connected_ && is_camera_configured_;
  return true;
}

bool PhotoNode::deletePictures(gphoto2_ros::DeletePictures::Request &req, gphoto2_ros::DeletePictures::Response &resp) {
  std::vector<std::string>::iterator str_it;

  std::string delimiter = "/", folder, filename;

  ros::Time t_begin = ros::Time::now();
  ros::Duration mean_time;
  mean_time.fromSec(0);
  //Pre treat all the data to get folder and file separated
  for(str_it = req.camera_paths.begin();
      str_it != req.camera_paths.end(); str_it++) {
    size_t pos;

    pos = str_it->find_last_of('/');
    folder = str_it->substr(0, pos+1);
    filename = str_it->substr(pos+1);

    CameraFilePath path;
    std::strcpy(path.name, filename.c_str());
    std::strcpy(path.folder, folder.c_str());
    camera_.delete_pictures(path);
  }
  resp.success = true;
  return true;
}

// This service must be running in a different thread all the time to recover the paths of the pictures that are taken
// it listen to the events coming from the camera on a loop, and save the path of the picture taken when the right events is coming
void PhotoNode::picturePathTimerCallback(const ros::TimerEvent&) {
  picturePathCheck();
}

void PhotoNode::picturePathCheck() {
  std::string path_to_file = camera_.get_picture_path(&photo_mutex_);
  if(path_to_file != "") {
    ROS_INFO("Adding picture path to list: %s", path_to_file.c_str());
    picture_path_list.push_back(path_to_file);
    std_msgs::String msg;
    msg.data = path_to_file;
    path_pub_.publish(msg);
  }
}

void PhotoNode::reinitCameraCallback(const ros::TimerEvent &) {

}

bool PhotoNode::isDeviceExist( std::string port_info){
  bool is_device_found =false;

  int bus_to_find=std::stoi(port_info.substr (4,3));
  int device_to_find=std::stoi(port_info.substr (8,3));

  libusb_device **list;
  libusb_device *found = NULL;
  ssize_t cnt = libusb_get_device_list(NULL, &list);
  ssize_t i = 0;
  int err = 0;
  if (cnt < 0)
    libusb_exit(NULL);
  for (i = 0; i < cnt; i++) {
    libusb_device *device = list[i];
    int bus_nb=libusb_get_bus_number(device);
    int device_nb= libusb_get_device_address(device);
    //ROS_INFO_STREAM("Testing: Bus: " << bus_nb << ", Device: " << device_nb);
    if ((bus_nb == bus_to_find) && (device_nb == device_to_find)){
      //ROS_INFO_STREAM("Found: Bus: " << bus_nb << ", Device: " << device_nb);
      is_device_found=true;
      break;
    }
  }
  libusb_free_device_list(list, 1);
  return is_device_found;
}

//static struct libusb_device_handle *devh = NULL;
bool PhotoNode::isDeviceClaimed( std::string port_info){
  bool is_device_claimed =false;

  int bus_to_find=std::stoi(port_info.substr (4,3));
  int device_to_find=std::stoi(port_info.substr (8,3));

  libusb_device **list;
  libusb_device *found = NULL;
  libusb_device_handle *dhand = NULL;
  ssize_t cnt = libusb_get_device_list(NULL, &list);
  ssize_t i = 0;
  int err = 0;
  if (cnt < 0)
    libusb_exit(NULL);
  for (i = 0; i < cnt; i++) {
    libusb_device *device = list[i];
    int bus_nb=libusb_get_bus_number(device);
    int device_nb= libusb_get_device_address(device);
    //ROS_INFO_STREAM("Testing: Bus: " << bus_nb << ", Device: " << device_nb);
    if ((bus_nb == bus_to_find) && (device_nb == device_to_find)){
      libusb_open(device,&dhand);
      libusb_claim_interface(dhand,0);
      libusb_close(dhand);
      is_device_claimed=libusb_claim_interface(dhand,0)!=0;
      break;
    }
  }
  libusb_free_device_list(list, 1);
  return is_device_claimed;
}


int main(int argc, char **argv)
{
  ros::init(argc, argv, "photo_node");
  ros::AsyncSpinner spinner(4);
  PhotoNode a("set_focus_action", "trigger_action");
  spinner.start();


  ros::Rate r(10);
  r.sleep();
  while (ros::ok()) {
    if (a.is_camera_connected_){
      //ROS_INFO("Main: Testing cam connection");
      if (a.isDeviceExist(a.current_port_info)){
        //ROS_WARN_STREAM("Main: cam ok");
        //a.picturePathCheck();
      }else {
        ROS_WARN_STREAM("Camera [" << a.owner_ << "] DISCONNECTED on port ["<<  a.current_port_info <<"]");
        a.picture_path_timer_.stop();
        a.camera_.photo_camera_close();
        a.is_camera_connected_=false;
        a.is_camera_configured_=false;
        a.current_port_info="";
      }
    }else {
      //ROS_WARN_STREAM("Main :Attempting to reconnect");
      if (a.camera_initialization(a.owner_)){
        ROS_WARN_STREAM("Camera [" << a.owner_ << "] reconnected on port ["<<  a.current_port_info <<"], reconfiguring");
        a.camera_configs(a.aperture_mode_,a.shutter_speed_mode_, a.iso_mode_);
        a.picture_path_timer_.start();
      }else {
        ros::Duration(2.0).sleep();
      }
    }
    r.sleep();
  }
  ros::waitForShutdown();
  a.~PhotoNode();

}
