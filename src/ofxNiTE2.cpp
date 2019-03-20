#include "ofxNiTE2.h"

#include "utils/DepthRemapToRange.h"

#ifdef HAVE_NITE2

namespace ofxNiTE2
{
    void check_error(nite::Status rc)
    {
        if (rc == nite::STATUS_OK) {
            return;
        }
        ofLogError("ofxNiTE2") << openni::OpenNI::getExtendedError();
    }

	void init()
	{
		static bool inited = false;
		if (inited) return;
		inited = true;
		
        nite::Status status = nite::NiTE::initialize();
        if (status != nite::STATUS_OK)
        {
            ofLogError() << "Initialize failed:" << endl << openni::OpenNI::getExtendedError();
            ofExit(-1);
        }
        else {
            nite::Version version = nite::NiTE::getVersion();
            ofLogNotice() << "NITE initialised. Version: " << version.major << "." << version.minor << "." << version.maintenance << "." << version.build;

        }

	}
	
	class UserTracker;
}

using namespace ofxNiTE2;

#pragma mark - UserTracker

UserTracker::~UserTracker() {
    UserTracker::exit();
}

bool UserTracker::setup(ofxNI2::Device &device)
{
	ofxNiTE2::init();

    bSetup = false;
	
	this->device = &device;
	mutex = new ofMutex;
	
	{
        // get initial info from depth stream
		openni::VideoStream stream;
		stream.create(device, openni::SENSOR_DEPTH);
		
		float fov = stream.getVerticalFieldOfView();
		overlay_camera.setFov(ofRadToDeg(fov));
		overlay_camera.setNearClip(500);

        depthWidth = stream.getVideoMode().getResolutionX();
        depthHeight = stream.getVideoMode().getResolutionY();
		
		stream.destroy();
	}
	
	openni::Device &dev = device;
	check_error(user_tracker.create(&dev));
	if (!user_tracker.isValid()) return false;
	
	user_tracker.addNewFrameListener(this);
    user_tracker.setSkeletonSmoothingFactor(0.5);

    pix.allocate(depthWidth, depthHeight, 1);
	
	ofAddListener(device.updateDevice, this, &UserTracker::onUpdate);
    bSetup = true;
    bTrackOutOfFrame = false;
	
	return true;
}

void UserTracker::exit()
{
    if(bSetup) {

        userTrackerFrame.release();
        ofRemoveListener(device->updateDevice, this, &UserTracker::onUpdate);

        map<nite::UserId, User::Ref>::iterator it = users.begin();
        while (it != users.end())
        {
            user_tracker.stopSkeletonTracking(it->first);
            it++;
        }

        users.clear();

        if (user_tracker.isValid())
        {
            user_tracker.removeNewFrameListener(this);
            user_tracker.destroy();
        }

        bSetup = false;
    }

}

void UserTracker::clear()
{
	for (int i = 0; i < users_data.size(); i++)
	{
		const nite::UserData& user = users_data[i];
		user_tracker.stopSkeletonTracking(user.getId());
	}
	
	users_data.clear();
	
	users.clear();
	users_arr.clear();
}

void UserTracker::onNewFrame(nite::UserTracker &tracker)
{
	nite::Status rc = tracker.readFrame(&userTrackerFrame);
	
	if (rc != nite::STATUS_OK)
	{
		check_error(rc);
		return;
	}

    // calculate FPS of tracker
    lastFrame = newFrame;
    newFrame = ofGetElapsedTimeMillis();
    float delta  = newFrame - lastFrame;
    float newfps = 1.0f/(delta/1000.0f);
    fps = 0.8f*fps + 0.2f*newfps;
	
	user_map = userTrackerFrame.getUserMap();
	
	mutex->lock();
	{
		const nite::Array<nite::UserData>& users = userTrackerFrame.getUsers();
		for (int i = 0; i < users.getSize(); i++)
			users_data.push_back(users[i]);
	}
	mutex->unlock();
	
	{
		openni::VideoFrameRef frame = userTrackerFrame.getDepthFrame();
		
		const unsigned short *pixels = (const unsigned short*)frame.getData();
		int w = frame.getVideoMode().getResolutionX();
		int h = frame.getVideoMode().getResolutionY();
		int num_pixels = w * h;
		
		pix.allocate(w, h, 1);
		pix.getBackBuffer().setFromPixels(pixels, w, h, OF_IMAGE_GRAYSCALE);
		pix.swap();
	}
}

ofPixels UserTracker::getPixelsRef(int _near, int _far, bool invert)
{
	ofPixels pix;
	ofxNI2::depthRemapToRange(getPixelsRef(), pix, _near, _far, invert);
	return pix;
}

void UserTracker::onUpdate(ofEventArgs&)
{
	mutex->lock();
	
	if (!users_data.empty())
	{
		users_arr.clear();
		
		for (int i = 0; i < users_data.size(); i++)
		{
			const nite::UserData& user = users_data[i];
			
			User::Ref user_ptr;
			bool has_user = users.find(user.getId()) != users.end();
			
			if (user.isNew())
			{
				user_ptr = User::Ref(new User);
				users[user.getId()] = user_ptr;
				user_tracker.startSkeletonTracking(user.getId());
                startTrackTime = ofGetElapsedTimeMillis();
                bShowDelta = true;
			}
			else if (has_user)
			{
                if ((user.isLost()) || (!(user.isVisible()) && (!bTrackOutOfFrame)) )
				{
					// emit lost user event
					ofNotifyEvent(lostUser, users[user.getId()], this);
                    ofLogNotice("ofxNite") << "Lost user...";
					
					user_tracker.stopSkeletonTracking(user.getId());
					users.erase(user.getId());
					continue;
                }
				else
				{
					user_ptr = users[user.getId()];

                    if(user.getSkeleton().getState() == nite::SKELETON_TRACKED )
                    {
                        if(bShowDelta) {
                            float deltaTime = (ofGetElapsedTimeMillis() - startTrackTime)/1000.0f;
                            ofLogNotice("ofxNiTE2") << "User found in " << deltaTime << " secs";
                            bShowDelta = false;
                        }
                    }
				}
			}
			
			if (!user_ptr) continue;
			
            user_ptr->updateUserData(user,user_tracker);
			users_arr.push_back(user_ptr);
			
			if (user.isNew())
			{
				// emit new user event
				ofNotifyEvent(newUser, user_ptr, this);
                ofLogNotice("ofxNiTE2") << "New user id: " << user.getId();
			}
		}
		
		users_data.clear();
	}
	
	mutex->unlock();
}

void UserTracker::draw()
{
	map<nite::UserId, User::Ref>::iterator it = users.begin();
	while (it != users.end())
	{
        it->second->draw();
		it++;
	}
}

void UserTracker::draw3D()
{
    map<nite::UserId, User::Ref>::iterator it = users.begin();
    while (it != users.end())
    {
        it->second->draw3D();
        it++;
    }
}

#pragma mark - User

void User::updateUserData(const nite::UserData& data, const nite::UserTracker& tracker)
{
	userdata = data;
	
	for (int i = 0; i < NITE_JOINT_COUNT; i++)
	{
        const nite::SkeletonJoint &joint = data.getSkeleton().getJoint((nite::JointType)i);
        joints[i].updateJointData(joint);
	}

	
	stringstream ss;
	ss << "[" << data.getId() << "]" << endl;
	
	ss << (data.isVisible() ? "Visible" : "Out of Scene") << endl;;
	
	switch (data.getSkeleton().getState())
	{
		case nite::SKELETON_NONE:
			ss << "Stopped tracking.";
			break;
		case nite::SKELETON_CALIBRATING:
			ss << "Calibrating...";
			break;
		case nite::SKELETON_TRACKED:
			ss << "Tracking!";            
            {
                float x, y;
                nite::SkeletonJoint joint = data.getSkeleton().getJoint(nite::JOINT_HEAD);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &head.x, &head.y);
                 //= ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_NECK);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                neck = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_LEFT_SHOULDER);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                leftShoulder = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_RIGHT_SHOULDER);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                rightShoulder = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_LEFT_ELBOW);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                leftElbow = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_RIGHT_ELBOW);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                rightElbow = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_LEFT_HAND);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                leftHand = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_RIGHT_HAND);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                rightHand = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_TORSO);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                torso = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_LEFT_HIP);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                leftHip = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_RIGHT_HIP);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                rightHip = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_LEFT_KNEE);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                leftKnee = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_RIGHT_KNEE);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                rightKnee = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_LEFT_FOOT);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                leftFoot = ofVec2f(x,y);

                joint = data.getSkeleton().getJoint(nite::JOINT_RIGHT_FOOT);
                tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);
                rightFoot = ofVec2f(x,y);
            }
			break;
		case nite::SKELETON_CALIBRATION_ERROR_NOT_IN_POSE:
		case nite::SKELETON_CALIBRATION_ERROR_HANDS:
		case nite::SKELETON_CALIBRATION_ERROR_LEGS:
		case nite::SKELETON_CALIBRATION_ERROR_HEAD:
		case nite::SKELETON_CALIBRATION_ERROR_TORSO:
			ss << "Calibration Failed... :-|";
			break;
	}
	
	status_string = ss.str();
	
	const nite::Point3f& pos = userdata.getCenterOfMass();
	center_of_mass.set(pos.x, pos.y, -pos.z);
	
    Joint &_torso = joints[nite::JOINT_TORSO];
	activity += (_torso.getPosition().distance(center_of_bone) - activity) * 0.1;
    center_of_bone = _torso.getPosition();
}

void User::draw()
{
    ofPushStyle();
    float r = 3;
    ofDrawCircle(head,          r);
    ofDrawCircle(neck,          r);
    ofDrawCircle(leftShoulder,  r);
    ofDrawCircle(rightShoulder, r);
    ofDrawCircle(leftElbow,     r);
    ofDrawCircle(rightElbow,    r);
    ofDrawCircle(leftHand,      r);
    ofDrawCircle(rightHand,     r);
    ofDrawCircle(torso,         r);
    ofDrawCircle(leftHip,       r);
    ofDrawCircle(rightHip,      r);
    ofDrawCircle(leftKnee,      r);
    ofDrawCircle(rightKnee,     r);
    ofDrawCircle(leftFoot,      r);
    ofDrawCircle(rightFoot,     r);
    ofDrawLine(head,            neck);
    ofDrawLine(leftShoulder,    rightShoulder);
    ofDrawLine(leftShoulder,    torso);
    ofDrawLine(rightShoulder,   torso);
    ofDrawLine(leftShoulder,    leftElbow);
    ofDrawLine(leftElbow,       leftHand);
    ofDrawLine(rightShoulder,   rightElbow);
    ofDrawLine(rightElbow,      rightHand);
    ofDrawLine(torso,           leftHip);
    ofDrawLine(torso,           rightHip);
    ofDrawLine(leftHip,         leftKnee);
    ofDrawLine(leftKnee,        leftFoot);
    ofDrawLine(rightHip,        rightKnee);
    ofDrawLine(rightKnee,       rightFoot);

    ofDrawBitmapString(status_string, center_of_mass);
    ofPopStyle();
}

void User::draw3D()
{
    ofPushStyle();
    for (int i = 0; i < joints.size(); i++)
    {
        joints[i].draw();
    }
    ofDrawBitmapString(status_string, center_of_mass);
    ofPopStyle();

}

ofVec2f User::getJointInDepthCoordinates(nite::UserData user, nite::JointType jointType, nite::UserTracker tracker)
{
  const nite::SkeletonJoint& joint = user.getSkeleton().getJoint(jointType);
  float x, y;

  tracker.convertJointCoordinatesToDepth(joint.getPosition().x, joint.getPosition().y, joint.getPosition().z, &x, &y);

  return ofVec2f(x, y);
}

void User::buildSkeleton()
{
	joints.resize(NITE_JOINT_COUNT);
	
	using namespace nite;
	
#define BIND(parent, child) joints[child].setParent(joints[parent]);
	
	BIND(JOINT_TORSO, JOINT_NECK);
	BIND(JOINT_NECK, JOINT_HEAD);
	
	BIND(JOINT_TORSO, JOINT_LEFT_SHOULDER);
	BIND(JOINT_LEFT_SHOULDER, JOINT_LEFT_ELBOW);
	BIND(JOINT_LEFT_ELBOW, JOINT_LEFT_HAND);
	
	BIND(JOINT_TORSO, JOINT_RIGHT_SHOULDER);
	BIND(JOINT_RIGHT_SHOULDER, JOINT_RIGHT_ELBOW);
	BIND(JOINT_RIGHT_ELBOW, JOINT_RIGHT_HAND);
	
	BIND(JOINT_TORSO, JOINT_LEFT_HIP);
	BIND(JOINT_LEFT_HIP, JOINT_LEFT_KNEE);
	BIND(JOINT_LEFT_KNEE, JOINT_LEFT_FOOT);
	
	BIND(JOINT_TORSO, JOINT_RIGHT_HIP);
	BIND(JOINT_RIGHT_HIP, JOINT_RIGHT_KNEE);
	BIND(JOINT_RIGHT_KNEE, JOINT_RIGHT_FOOT);
	
#undef BIND
	
}

#pragma mark - Joint

inline static void billboard()
{
    ofMatrix4x4 m;
    glGetFloatv(GL_MODELVIEW_MATRIX, m.getPtr());
    
    ofVec3f s = m.getScale();
    
    m(0, 0) = s.x;
    m(0, 1) = 0;
    m(0, 2) = 0;
    
    m(1, 0) = 0;
    m(1, 1) = s.y;
    m(1, 2) = 0;
    
    m(2, 0) = 0;
    m(2, 1) = 0;
    m(2, 2) = s.z;
    
    glLoadMatrixf(m.getPtr());
}

void Joint::draw()
{
	ofNode *parent = getParent();
	
	if (parent)
	{
		parent->transformGL();
        ofDrawLine(ofVec3f(0, 0, 0), getPosition());
		parent->restoreTransformGL();
	}

	transformGL();
	ofDrawAxis(100);
	
	billboard();
	
	ofPushStyle();
	ofFill();
	ofSetColor(255);
    ofDrawCircle(0, 0, 20 * getPositionConfidence());
	ofPopStyle();
	
	restoreTransformGL();
}

void Joint::updateJointData(const nite::SkeletonJoint& data)
{
	joint = data;
	
	const nite::Point3f& pos = data.getPosition();
	const nite::Quaternion& rot = data.getOrientation();
	
	setGlobalOrientation(ofQuaternion(-rot.x, -rot.y, rot.z, rot.w));
	setGlobalPosition(pos.x, pos.y, -pos.z);
}

#endif
