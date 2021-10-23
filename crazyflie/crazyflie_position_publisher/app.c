// Added Tf publisher to the official demo.
// https://github.com/micro-ROS/freertos_apps/blob/galactic/apps/crazyflie_position_publisher/app.c#L57
#include "FreeRTOS.h"
#include "task.h"

#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <geometry_msgs/msg/point32.h>
#include <micro_ros_utilities/type_utilities.h>

#include <rcutils/allocator.h>
#include <rmw_microros/rmw_microros.h>
#include <rmw_microros/ping.h>

#include "config.h"
#include "log.h"
#include "worker.h"
#include "num.h"
#include "debug.h"
#include <time.h>

#include "microrosapp.h"

#define RCCHECK(fn) { rcl_ret_t temp_rc = fn; if((temp_rc != RCL_RET_OK)){DEBUG_PRINT("Failed status on line %d: %d. Aborting.\n",__LINE__,(int)temp_rc);vTaskDelete(NULL);}}
#define RCSOFTCHECK(fn) { rcl_ret_t temp_rc = fn; if((temp_rc != RCL_RET_OK)){DEBUG_PRINT("Failed status on line %d: %d. Continuing.\n",__LINE__,(int)temp_rc);}}

static uint8_t crtp_buffer[CRTP_BUFFER_SIZE];

rcl_publisher_t publisher_odometry;
rcl_publisher_t publisher_attitude;
// Create publisher 3
rcl_publisher_t pub_tf;
// Create publisher 4
// rcl_publisher_t pub_pose;

static int pitchid, rollid, yawid;
static int Xid, Yid, Zid;
static float posX, posY, posZ; 
static int qxid, qyid, qzid, qwid;

float sign(float x){
    return (x >= 0) ? 1.0 : -1.0;
}

void appMain(){
    absoluteUsedMemory = 0;
    usedMemory = 0;

    // ####################### MICROROS INIT #######################
    DEBUG_PRINT("Free heap pre uROS: %d bytes\n", xPortGetFreeHeapSize());
    vTaskDelay(50);

    rcl_allocator_t freeRTOS_allocator = rcutils_get_zero_initialized_allocator();
    freeRTOS_allocator.allocate = __crazyflie_allocate;
    freeRTOS_allocator.deallocate = __crazyflie_deallocate;
    freeRTOS_allocator.reallocate = __crazyflie_reallocate;
    freeRTOS_allocator.zero_allocate = __crazyflie_zero_allocate;

    if (!rcutils_set_default_allocator(&freeRTOS_allocator)) {
        DEBUG_PRINT("Error on default allocators (line %d)\n",__LINE__);
        vTaskSuspend( NULL );
    }

    transport_args custom_args = { .radio_channel = 65, .radio_port = 9, .crtp_buffer = &crtp_buffer[0] };
    rmw_uros_set_custom_transport( 
        true, 
        (void *) &custom_args, 
        crazyflie_serial_open, 
        crazyflie_serial_close, 
        crazyflie_serial_write, 
        crazyflie_serial_read
    ); 

    // Wait for available agent
    while(RMW_RET_OK != rmw_uros_ping_agent(1000, 10))
    {
        vTaskDelay(100/portTICK_RATE_MS);
    }

    rcl_allocator_t allocator = rcl_get_default_allocator();
	rclc_support_t support;

	// create init_options
	RCCHECK(rclc_support_init(&support, 0, NULL, &allocator));

	// create node
	rcl_node_t node;
	RCCHECK(rclc_node_init_default(&node, "crazyflie_node", "", &support));

	// create publishers
	RCCHECK(rclc_publisher_init_best_effort(&publisher_odometry, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Point32), "/drone/odometry"));
	RCCHECK(rclc_publisher_init_best_effort(&publisher_attitude, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Point32), "/drone/attitude"));
    RCCHECK(rclc_publisher_init_best_effort(&pub_tf, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, TransformStamped),　"/drone/tf"))
    // RCCHECK(rclc_publisher_init_best_effort(&pub_pose, &node,
    //     ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, PoseStamped), "/drone/pose"));

    // Init messages
    geometry_msgs__msg__Point32 pose;
    geometry_msgs__msg__Point32 odom;
    geometry_msgs__msg__TransformStamped tf;
    // geometry_msgs__msg__PoseStamped pose;

    static micro_ros_utilities_memory_conf_t conf = {0};

    bool success = micro_ros_utilities_create_message_memory(
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Point32),
        &pose,
        conf);

    success &= micro_ros_utilities_create_message_memory(
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Point32),
        &odom,
        conf);

    
    success &= micro_ros_utilities_create_message_memory(
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, TransformStamped),
        &tf,
        conf));

    // success &= micro_ros_utilities_create_message_memory(
    //     ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, PoseStamped),
    //     &pose,
    //     conf));

    if (!success)
    {
        DEBUG_PRINT("Memory allocation for /drone messages failed\n");
        return;
    }

    // tf configuration
    tf.child_frame_id = micro_ros_string_utilities_set(tf.child_frame_id, "/base_footprint_drone");
    tf.header.frame_id = micro_ros_string_utilities_set(tf.header.frame_id, "/map");
    // pose.header.frame_id = micro_ros_string_utilities_set(pose.header.frame_id, "/map");

    // Get quaternion
    qxid = logGetVarId("stateEstimate", "qx");
    qyid = logGetVarId("stateEstimate", "qy");
    qzid = logGetVarId("stateEstimate", "qz");
    qwid = logGetVarId("stateEstimate", "qw");

    //Get pitch, roll and yaw value
    pitchid = logGetVarId("stateEstimate", "pitch");
    rollid = logGetVarId("stateEstimate", "roll");
    yawid = logGetVarId("stateEstimate", "yaw");

    //Get X,Y and Z value
    Xid = logGetVarId("stateEstimate", "x");
    Yid = logGetVarId("stateEstimate", "y");
    Zid = logGetVarId("stateEstimate", "z");

    DEBUG_PRINT("Free heap post uROS configuration: %d bytes\n", xPortGetFreeHeapSize());
    DEBUG_PRINT("uROS Used Memory %d bytes\n", usedMemory);
    DEBUG_PRINT("uROS Absolute Used Memory %d bytes\n", absoluteUsedMemory);

	while(1) {
        // publisher_attitude
        pose.x = logGetFloat(pitchid);
        pose.y = logGetFloat(rollid);
        pose.z = logGetFloat(yawid);

        // publisher_odometry
        odom.x = logGetFloat(Xid);
        odom.y = logGetFloat(Yid);
        odom.z = logGetFloat(Zid);

        // pub_tf
        tf.transform.rotation.x = logGetFloat(qxid);
        tf.transform.rotation.y = logGetFloat(qyid);
        tf.transform.rotation.z = logGetFloat(qzid);
        tf.transform.rotation.w = logGetFloat(qwid);

        // pub_pose & pub_tf
        posX = logGetFloat(Xid);
        posY = logGetFloat(Yid);
        posZ = logGetFloat(Zid);

        tf.transform.translation.x = posX;
        tf.transform.translation.y = posY;
        tf.transform.translation.z = posZ;

        // pose.pose.position.x = posX;
        // pose.pose.position.y = posY;
        // pose.pose.position.z = posZ;

        RCSOFTCHECK(rcl_publish( &publisher_attitude, (const void *) &pose, NULL));
        RCSOFTCHECK(rcl_publish( &publisher_odometry, (const void *) &odom, NULL));
        RCSOFTCHECK(rcl_publish( &pub_tf, &tf, NULL));
        // RCSOFTCHECK(rcl_publish( &pub_pose, &pose, NULL));

        vTaskDelay(10/portTICK_RATE_MS);
	}

    success = micro_ros_utilities_destroy_message_memory(
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Point32),
        &pose,
        conf
    );

    success &= micro_ros_utilities_destroy_message_memory(
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Point32),
        &odom,
        conf
    );

    if (!success)
    {
        DEBUG_PRINT("Memory release for /drone messages failed\n");
        return;
    }

	RCCHECK(rcl_publisher_fini(&publisher_attitude, &node))
	RCCHECK(rcl_publisher_fini(&publisher_odometry, &node))
    RCCHECK(rcl_publisher_fini(&pub_tf, &node))
	RCCHECK(rcl_node_fini(&node))

    vTaskSuspend( NULL );
}