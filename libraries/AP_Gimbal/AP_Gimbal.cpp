// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

#include <stdio.h>
#include <AP_Common.h>
#include <AP_Progmem.h>
#include <AP_Param.h>
#include <AP_Gimbal.h>
#include <GCS.h>
#include <GCS_MAVLink.h>
#include <AP_SmallEKF.h>

const AP_Param::GroupInfo AP_Gimbal::var_info[] PROGMEM = {
    AP_GROUPEND
};

uint16_t feedback_error_count;
static float K_gimbalRate = 0.1f;
static float angRateLimit = 0.5f;

void AP_Gimbal::receive_feedback(mavlink_message_t *msg)
{
    update_targets_from_rc();
    decode_feedback(msg);
    update_state();
    if (_ekf.getStatus()){
        send_control();
    }
}
    

void AP_Gimbal::decode_feedback(mavlink_message_t *msg)
{
    mavlink_gimbal_feedback_t feedback_msg;
    uint8_t expected_id = _measurament.id +1;
    mavlink_msg_gimbal_feedback_decode(msg, &feedback_msg);
    _measurament.id = feedback_msg.id;

    if(expected_id !=_measurament.id){
        feedback_error_count++;
        ::printf("error count: %d\n", feedback_error_count);
    }

    _measurament.delta_angles.x = feedback_msg.gyrox;
    _measurament.delta_angles.y = feedback_msg.gyroy,
    _measurament.delta_angles.z = feedback_msg.gyroz;
    _measurament.delta_velocity.x = feedback_msg.accx,
    _measurament.delta_velocity.y = feedback_msg.accy,
    _measurament.delta_velocity.z = feedback_msg.accz;   
    _measurament.joint_angles.x = feedback_msg.joint_roll;
    _measurament.joint_angles.y = feedback_msg.joint_el,
    _measurament.joint_angles.z = feedback_msg.joint_az;

    //apply joint angle compensation
    _measurament.joint_angles -= _joint_offsets;
}


// convert the quaternion to rotation vector
Vector3f AP_Gimbal::quaternion_to_vector(Quaternion quat)
{
        Vector3f vector;
        float scaler = 1.0f-quat[0]*quat[0];
        if (scaler > 1e-12) {
            scaler = 1.0f/sqrtf(scaler);
            if (quat[0] < 0.0f) {
                scaler *= -1.0f;
            }
            vector.x = quat[1] * scaler;
            vector.y = quat[2] * scaler;
            vector.z = quat[3] * scaler;
        } else {
            vector.zero();
        }
    return vector;
}


// Define rotation matrix using a 312 rotation sequence vector
Matrix3f AP_Gimbal::vetor312_to_rotation_matrix(Vector3f vector)
{
        Matrix3f matrix;
        float cosPhi = cosf(vector.x);
        float cosTheta = cosf(vector.y);
        float sinPhi = sinf(vector.x);
        float sinTheta = sinf(vector.y);
        float sinPsi = sinf(vector.z);
        float cosPsi = cosf(vector.z);
        matrix[0][0] = cosTheta*cosPsi-sinPsi*sinPhi*sinTheta;
        matrix[1][0] = -sinPsi*cosPhi;
        matrix[2][0] = cosPsi*sinTheta+cosTheta*sinPsi*sinPhi;
        matrix[0][1] = cosTheta*sinPsi+cosPsi*sinPhi*sinTheta;
        matrix[1][1] = cosPsi*cosPhi;
        matrix[2][1] = sinPsi*sinTheta-cosTheta*cosPsi*sinPhi;
        matrix[0][2] = -sinTheta*cosPhi;
        matrix[1][2] = sinPhi;
        matrix[2][2] = cosTheta*cosPhi;
        return matrix;
}

void AP_Gimbal::update_state()
{
    // Run the gimbal attitude and gyro bias estimator
    _ekf.RunEKF(delta_time, _measurament.delta_angles, _measurament.delta_velocity, _measurament.joint_angles);

    // get the gimbal quaternion estimate
    Quaternion quatEst;
    _ekf.getQuat(quatEst);
 
    // Add the control rate vectors
    gimbalRateDemVec = getGimbalRateDemVecYaw(quatEst) + getGimbalRateDemVecTilt(quatEst) + getGimbalRateDemVecForward(quatEst);

    //Compensate for gyro bias
    //TODO send the gyro bias to the gimbal        
    Vector3f gyroBias;
    _ekf.getGyroBias(gyroBias);
    gimbalRateDemVec+= gyroBias;
}

Vector3f AP_Gimbal::getGimbalRateDemVecYaw(Quaternion quatEst)
{
        // Define rotation from vehicle to gimbal using a 312 rotation sequence
        Matrix3f Tvg = vetor312_to_rotation_matrix(_measurament.joint_angles);

        // multiply the yaw joint angle by a gain to calculate a demanded vehicle frame relative rate vector required to keep the yaw joint centred
        Vector3f gimbalRateDemVecYaw;
        gimbalRateDemVecYaw.z = - K_gimbalRate * _measurament.joint_angles.z;

        // Get filtered vehicle turn rate in earth frame
        vehicleYawRateFilt = (1.0f - yawRateFiltPole * delta_time) * vehicleYawRateFilt + yawRateFiltPole * delta_time * _ahrs.get_yaw_rate_earth();
        Vector3f vehicle_rate_ef(0,0,vehicleYawRateFilt);

         // calculate the maximum steady state rate error corresponding to the maximum permitted yaw angle error
        float maxRate = K_gimbalRate * yawErrorLimit;
        float vehicle_rate_mag_ef = vehicle_rate_ef.length();
        float excess_rate_correction = fabs(vehicle_rate_mag_ef) - maxRate; 
        if (vehicle_rate_mag_ef > maxRate) {
            if (vehicle_rate_ef.z>0.0f){
                gimbalRateDemVecYaw += _ahrs.get_dcm_matrix().transposed()*Vector3f(0,0,excess_rate_correction);    
            }else{
                gimbalRateDemVecYaw -= _ahrs.get_dcm_matrix().transposed()*Vector3f(0,0,excess_rate_correction);    
            }            
        }        

        // rotate into gimbal frame to calculate the gimbal rate vector required to keep the yaw gimbal centred
        gimbalRateDemVecYaw = Tvg * gimbalRateDemVecYaw;
        return gimbalRateDemVecYaw;
}

Vector3f AP_Gimbal::getGimbalRateDemVecTilt(Quaternion quatEst)
{
        // Calculate the gimbal 321 Euler angle estimates relative to earth frame
        Vector3f eulerEst;
        quatEst.to_euler(eulerEst.x, eulerEst.y, eulerEst.z);

        // Calculate a demanded quaternion using the demanded roll and pitch and estimated yaw (yaw is slaved to the vehicle)
        Quaternion quatDem;
        //TODO receive target from AP_Mount
        quatDem.from_euler(0, _angle_ef_target_rad.y, eulerEst.z);

       //divide the demanded quaternion by the estimated to get the error
        Quaternion quatErr = quatDem / quatEst;

        // multiply the angle error vector by a gain to calculate a demanded gimbal rate required to control tilt
        Vector3f gimbalRateDemVecTilt = quaternion_to_vector(quatErr) * K_gimbalRate;
        return gimbalRateDemVecTilt;
}

Vector3f AP_Gimbal::getGimbalRateDemVecForward(Quaternion quatEst)
{
        // quaternion demanded at the previous time step
        static Quaternion lastQuatDem;

        // calculate the delta rotation from the last to the current demand where the demand does not incorporate the copters yaw rotation
        Quaternion quatDemForward;
        quatDemForward.from_euler(0, _angle_ef_target_rad.y, 0);
        Quaternion deltaQuat = quatDemForward / lastQuatDem;
        lastQuatDem = quatDemForward;

        // convert to a rotation vector and divide by delta time to obtain a forward path rate demand
        Vector3f gimbalRateDemVecForward = quaternion_to_vector(deltaQuat) * (1.0f / delta_time);
        return gimbalRateDemVecForward;
}


void AP_Gimbal::send_control()
{
    mavlink_message_t msg;
    mavlink_gimbal_control_t control;
    control.target_system = _sysid;
    control.target_component = _compid;
    control.id = _measurament.id;

    control.ratex = gimbalRateDemVec.x;
    control.ratey = gimbalRateDemVec.y;
    control.ratez = gimbalRateDemVec.z;

    mavlink_msg_gimbal_control_encode(1, 1, &msg, &control);
    GCS_MAVLINK::routing.forward(&msg);
}

// returns the angle (degrees*100) that the RC_Channel input is receiving
int32_t angle_input(RC_Channel* rc, int16_t angle_min, int16_t angle_max)
{
    return (rc->get_reverse() ? -1 : 1) * (rc->radio_in - rc->radio_min) * (int32_t)(angle_max - angle_min) / (rc->radio_max - rc->radio_min) + (rc->get_reverse() ? angle_max : angle_min);
}

// returns the angle (radians) that the RC_Channel input is receiving
float angle_input_rad(RC_Channel* rc, int16_t angle_min, int16_t angle_max)
{
    return radians(angle_input(rc, angle_min, angle_max)*0.01f);
}

// update_targets_from_rc - updates angle targets using input from receiver
void AP_Gimbal::update_targets_from_rc()
{
    float tilt = angle_input_rad(RC_Channel::rc_channel(tilt_rc_in-1), _tilt_angle_min, _tilt_angle_max);
    float rate = (tilt - _angle_ef_target_rad.y) / delta_time;
    if(rate > _max_tilt_rate){
        _angle_ef_target_rad.y += delta_time*_max_tilt_rate;
    }else if(rate < -_max_tilt_rate){
        _angle_ef_target_rad.y -= delta_time*_max_tilt_rate;
    }else{
        _angle_ef_target_rad.y = tilt;
    }
}
