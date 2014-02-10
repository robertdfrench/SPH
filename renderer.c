#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "circles_gl.h"
#include "mpi.h"
#include "communication.h"
#include "fluid.h"
#include "font_gl.h"

void start_renderer()
{
    // Setup initial OpenGL ES state
    // OpenGL state
    GL_STATE_T gl_state;
    memset(&gl_state, 0, sizeof(GL_STATE_T));

    // Start OpenGL
    RENDER_T render_state;
    init_ogl(&gl_state, &render_state);

    // Initialize circle OpenGL state
    CIRCLE_T circle_state;
    init_circles(&circle_state);

    // Initialize font atlas
    FONT_T font_state;
    init_font(&font_state, gl_state.screen_width, gl_state.screen_height);

    // Number of processes
    int num_procs, num_compute_procs;
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
    num_compute_procs = num_procs - 1;

    // Allocate array of paramaters
    // So we can use MPI_Gather instead of MPI_Gatherv
    tunable_parameters *node_params = malloc(num_compute_procs*sizeof(tunable_parameters));

    // Setup render state
    render_state.node_params = node_params;
    render_state.num_compute_procs = num_compute_procs;
    render_state.selected_parameter = 0;

    int i,j;

    // Broadcast pixels rati
    short pixel_dims[2];
    pixel_dims[0] = (short)gl_state.screen_width;
    pixel_dims[1] = (short)gl_state.screen_height;
    MPI_Bcast(pixel_dims, 2, MPI_SHORT, 0, MPI_COMM_WORLD);
 
    // Recv world dimensions from global rank 1
    float world_dims[2];
    MPI_Recv(world_dims, 2, MPI_FLOAT, 1, 8, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    // Receive number of global particles
    int max_particles;
    MPI_Recv(&max_particles, 1, MPI_INT, 1, 9, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // Calculate world unit to pixel
    float world_to_pix_scale = gl_state.screen_width/world_dims[0];
    printf("global particles: %d\n", max_particles);

    // Gatherv initial tunable parameters values
    int *param_counts = malloc(num_procs * sizeof(int));
    int *param_displs = malloc(num_procs * sizeof(int));
    for(i=0; i<num_procs; i++) {
        param_counts[i] = i?1:0; // will not receive from rank 0
        param_displs[i] = i?i-1:0; // rank i will reside in params[i-1]
    }
    // Initial gather
    MPI_Gatherv(MPI_IN_PLACE, 0, TunableParamtype, node_params, param_counts, param_displs, TunableParamtype, 0, MPI_COMM_WORLD);

    // Allocate particle receive array
    int num_coords = 2;
    short *particle_coords = malloc(num_coords * max_particles*sizeof(short));

    // Allocate points array(position + color)
    int point_size = 5 * sizeof(float);
    float *points = (float*)malloc(point_size*max_particles);

    // Allocate mover point array(position + color)
    float mover_point[5];

    // Number of coordinates received from each proc
    int *particle_coordinate_counts = malloc(num_compute_procs * sizeof(int));
    // Keep track of order in which particles received
    int *particle_coordinate_ranks = malloc(num_compute_procs * sizeof(int));

    // Set background color
    glClearColor(0, 0, 0, 1);

    // Create color index - hard coded for now to experiment
    float colors_by_rank[9] = {0.69,0.07,0.07,
        1.0,1.0,0.1,
        0.08,0.52,0.8};

    int num_coords_rank;
    int current_rank, num_parts;
    float mouse_x, mouse_y, mouse_x_scaled, mouse_y_scaled;
    float mover_radius, mover_radius_scaled;

    int frames_per_fps = 30;
    int num_steps = 0;
    double current_time;
    double wall_time = MPI_Wtime();
    float fps=0.0f;

    // Setup MPI requests used to gather particle coordinates
    MPI_Request coord_reqs[num_compute_procs];
    int src, coords_recvd;
    float gl_x, gl_y;
    float particle_radius = 1.0f;

    MPI_Status status;

    while(1){
	// Every frames_per_fps steps calculate FPS
	if(num_steps%frames_per_fps == 0) {
	    current_time =  MPI_Wtime();
	    wall_time = current_time - wall_time;
	    fps = frames_per_fps/wall_time;
	    num_steps = 0;
	    wall_time = current_time;
	}	    

        // Send updated paramaters to compute nodes
        MPI_Scatterv(node_params, param_counts, param_displs, TunableParamtype, MPI_IN_PLACE, 0, TunableParamtype, 0, MPI_COMM_WORLD);

        // Get keyboard key press
        // process appropriately
        check_key_press(&gl_state);

        // Update mover position
        get_mouse(&mouse_x, &mouse_y, &gl_state);
        pixel_to_sim(world_dims, mouse_x, mouse_y, &mouse_x_scaled, &mouse_y_scaled);
        mover_radius = 1.0f;
        render_state.master_params.mover_center_x = mouse_x_scaled;
        render_state.master_params.mover_center_y = mouse_y_scaled;
        render_state.master_params.mover_radius = mover_radius;

	// Update all node parameters with master paramter values
        for(i=0; i<render_state.num_compute_procs; i++) {
            render_state.node_params[i].mover_center_x =  render_state.master_params.mover_center_x; 
            render_state.node_params[i].mover_center_y = render_state.master_params.mover_center_y;
            render_state.node_params[i].mover_radius = render_state.master_params.mover_radius;
	    render_state.node_params[i].g = render_state.master_params.g;
        }

        // Retrieve all particle coordinates (x,y)
	// Potentially probe is expensive? Could just allocated num_compute_procs*num_particles_global and async recv
	// OR do synchronous recv...very likely that synchronous receive is as fast as anything else
	coords_recvd = 0;
	for(i=0; i<num_compute_procs; i++) {
	    // Wait until message is ready from any proc
            MPI_Probe(MPI_ANY_SOURCE, 17, MPI_COMM_WORLD, &status);
	    // Retrieve probed values
    	    src = status.MPI_SOURCE;
            particle_coordinate_ranks[i] = src-1;
	    MPI_Get_count(&status, MPI_SHORT, &particle_coordinate_counts[src-1]); // src-1 to account for render node
	    // Start async recv using probed values
	    MPI_Irecv(particle_coords + coords_recvd, particle_coordinate_counts[src-1], MPI_SHORT, src, 17, MPI_COMM_WORLD, &coord_reqs[src-1]);
            // Update total number of floats recvd
            coords_recvd += particle_coordinate_counts[src-1];
	}

        // Clear background
        glClear(GL_COLOR_BUFFER_BIT);

        // Render mover
        sim_to_opengl(world_dims, mouse_x_scaled, mouse_y_scaled, &gl_x, &gl_y);
        mover_point[0] = gl_x;
        mover_point[1] = gl_y;
        mover_point[2] = 1.0f;
        mover_point[3] = 1.0f;
        mover_point[4] = 1.0f;
        mover_radius_scaled = mover_radius*world_to_pix_scale - particle_radius;
        update_mover_point(mover_point, mover_radius_scaled, &circle_state);

        // Draw FPS
        render_fps(&font_state, fps);

        // Draw font parameters
        // SHOULD JUST PASS IN render_state
        // RENDER STATE SHOULD INCLUDE PARAMETER VALUES TO DISPLAY
        render_parameters(&font_state, render_state.selected_parameter, render_state.master_params.g, 1.0f, 1.0f, 1.0f, 1.0f);

	// Wait for all coordinates to be received
	MPI_Waitall(num_compute_procs, coord_reqs, MPI_STATUSES_IGNORE);

        // Create points array (x,y,r,g,b)
	i = 0;
        current_rank = particle_coordinate_ranks[i];
        // j == coordinate pair
        for(j=0, num_parts=1; j<coords_recvd/2; j++, num_parts++) {
            if ( num_parts > particle_coordinate_counts[current_rank]/2){
                current_rank =  particle_coordinate_ranks[++i];
                num_parts = 1;
            }
            points[j*5]   = particle_coords[j*2]/(float)SHRT_MAX; 
            points[j*5+1] = particle_coords[j*2+1]/(float)SHRT_MAX;
            points[j*5+2] = colors_by_rank[3*current_rank];
            points[j*5+3] = colors_by_rank[3*current_rank+1];
            points[j*5+4] = colors_by_rank[3*current_rank+2];
        }

	// Draw particles
        update_points(points, particle_radius, coords_recvd/2, &circle_state);


        // Swap front/back buffers
        swap_ogl(&gl_state);

        num_steps++;

        // TODO: this function!
        // Calculate problem partitioning
//        balance_partitions(render_state);
    }

//    exit_ogl(&state.gl_state);

}

// Move selected parameter up
void move_parameter_up(RENDER_T *render_state)
{
    if(render_state->selected_parameter == MIN)
        render_state->selected_parameter = MAX;
    else
	render_state->selected_parameter--;
}

// Move selected parameter down
void move_parameter_down(RENDER_T *render_state) 
{
    if(render_state->selected_parameter == MAX)
        render_state->selected_parameter = MIN;
    else
        render_state->selected_parameter++;
}

void increase_parameter(RENDER_T *render_state)
{
    switch(render_state->selected_parameter) {
        case GRAVITY:
	   increase_gravity(render_state);
	    break;
    }
}

void decrease_parameter(RENDER_T *render_state)
{
    switch(render_state->selected_parameter) {
        case GRAVITY:
            decrease_gravity(render_state);
            break;
    }

}

// Increase gravity parameter
void increase_gravity(RENDER_T *render_state)
{
    static const float max_grav = -9.0;
    if(render_state->master_params.g < max_grav)
        return;

    int i;
    render_state->master_params.g -= 1.0;
}

// Decreate gravity parameter
void decrease_gravity(RENDER_T *render_state)
{
    static const float min_grav = 9.0;
    if(render_state->master_params.g > min_grav)
        return;

    int i;
    render_state->master_params.g += 1.0;
}

// Translate between pixel coordinates with origin at screen center
// to simulation coordinates
void pixel_to_sim(float *world_dims, float x, float y, float *sim_x, float *sim_y)
{
    float half_width = world_dims[0]*0.5;
    float half_height = world_dims[1]*0.5;

    *sim_x = x*half_width + half_width;
    *sim_y = y*half_height + half_height;
}

// Translate between simulation coordinates, origin bottom left, and opengl -1,1 center of screen coordinates
void sim_to_opengl(float *world_dims, float x, float y, float *gl_x, float *gl_y)
{
    float half_width = world_dims[0]*0.5;
    float half_height = world_dims[1]*0.5;

    *gl_x = x/half_width - 1.0;
    *gl_y = y/half_height - 1.0;
}
