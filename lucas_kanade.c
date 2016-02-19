/*
 * lucas_kanade.c
 *
 *  Created on: Jan 11, 2016
 *      Author: hrvoje
 */

/*
 * Copyright (C) 2014 G. de Croon
 *               2015 Freek van Tienen <freek.v.tienen@gmail.com>
 *
 * This file is part of Paparazzi.
 *
 * Paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * Paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Paparazzi; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

/**
 * @file modules/computer_vision/lib/vision/lucas_kanade.c
 * @brief efficient fixed-point optical-flow calculation
 *
 * - Initial fixed-point C implementation by G. de Croon
 * - Algorithm: Lucas-Kanade by Yves Bouguet
 * - Publication: http://robots.stanford.edu/cs223b04/algo_tracking.pdf
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "lucas_kanade.h"

/**
 * Compute the optical flow of several points using the Lucas-Kanade algorithm by Yves Bouguet
 * The initial fixed-point implementation is doen by G. de Croon and is adapted by
 * Freek van Tienen for the implementation in Paparazzi.
 * @param[in] *new_img The newest grayscale image (TODO: fix YUV422 support)
 * @param[in] *old_img The old grayscale image (TODO: fix YUV422 support)
 * @param[in] *points Points to start tracking from
 * @param[in,out] points_cnt The amount of points and it returns the amount of points tracked
 * @param[in] half_window_size Half the window size (in both x and y direction) to search inside
 * @param[in] subpixel_factor The subpixel factor which calculations should be based on
 * @param[in] max_iterations Maximum amount of iterations to find the new point
 * @param[in] step_threshold The threshold at which the iterations should stop
 * @param[in] max_points The maximum amount of points to track, we skip x points and then take a point.
 * @return The vectors from the original *points in subpixels
 */
struct flow_t *opticFlowLK(struct image_t *new_img, struct image_t *old_img, struct point_t *points, uint16_t *points_cnt, uint16_t half_window_size,
		uint32_t subpixel_factor, uint8_t max_iterations, uint8_t step_threshold, uint16_t max_points, uint8_t pyramid_level) {
	// A straightforward one-level implementation of Lucas-Kanade.
	// For all points:
	// (1) determine the subpixel neighborhood in the old image
	// (2) get the x- and y- gradients
	// (3) determine the 'G'-matrix [sum(Axx) sum(Axy); sum(Axy) sum(Ayy)], where sum is over the window
	// (4) iterate over taking steps in the image to minimize the error:
	//     [a] get the subpixel neighborhood in the new image
	//     [b] determine the image difference between the two neighborhoods
	//     [c] calculate the 'b'-vector
	//     [d] calculate the additional flow step and possibly terminate the iteration

	// Allocate some memory for returning the vectors
	struct flow_t *vectors = malloc(sizeof(struct flow_t) * max_points);

	// Allocate memory for image pyramids
	struct image_t *pyramid_old = (struct image_t *)malloc(sizeof(struct image_t) * (pyramid_level+1));
	struct image_t *pyramid_new = (struct image_t *)malloc(sizeof(struct image_t) * (pyramid_level+1));

	pyramid_build(old_img, pyramid_old, pyramid_level);
	pyramid_build(new_img, pyramid_new, pyramid_level);

	/*uint8_t show_level = pyramid_level;
	uint8_t *buff_pointer = (uint8_t *)pyramid_old[show_level].buf;
	printf("\nPyramid level %d \n", show_level);
	for (uint16_t j = 0; j != (pyramid_old[show_level].w * pyramid_old[show_level].h); j++){
		printf("%4d ", *buff_pointer++);
		if (!((j+1)%pyramid_old[show_level].w))
			printf("\n");
	}
	printf("width %u, height %u \n", pyramid_old[show_level].w, pyramid_old[show_level].h);
	*/

	// determine patch sizes and initialize neighborhoods
	uint16_t patch_size = 2 * half_window_size + 1; //CHANGED to put pixel in center, doesnt seem to impact results much, keep in mind.
	uint32_t error_threshold = (25 * 25) * (patch_size * patch_size);
	uint16_t padded_patch_size = patch_size + 2;
	// 3 values related to tracking window size, wont overflow

	// Create the window images
	struct image_t window_I, window_J, window_DX, window_DY, window_diff;
	image_create(&window_I, padded_patch_size, padded_patch_size, IMAGE_GRAYSCALE);
	image_create(&window_J, patch_size, patch_size, IMAGE_GRAYSCALE);
	image_create(&window_DX, patch_size, patch_size, IMAGE_GRADIENT);
	image_create(&window_DY, patch_size, patch_size, IMAGE_GRADIENT);
	image_create(&window_diff, patch_size, patch_size, IMAGE_GRADIENT);

	uint8_t exp = 1;
	for (uint8_t k = 0; k != pyramid_level; k++)
			exp *= 2;

	for (int8_t LVL = pyramid_level; LVL != -1; LVL--) {

		uint16_t points_orig = *points_cnt;
		*points_cnt = 0;
		//new_p, points_cnt are related to number of points, wont overflow

		// Calculate the amount of points to skip
		//float skip_points =	(points_orig > max_points) ? points_orig / max_points : 1;
		//printf("\nBased on max_points input, I'm skipping %f points(1 == none). \n", skip_points); //ADDED
		//CONC : I don't want to skip any points and result of skip_points is then appropriate
		uint16_t new_p = 0;
		// Go through all points
		for (uint16_t i = 0; i < max_points && i < points_orig; i++) {

			uint16_t p = i ;//* skip_points;


			if (LVL == pyramid_level){

				// If the pixel is outside ROI, do not track it
				if (points[p].x < half_window_size || (pyramid_old[LVL].w - points[p].x) < half_window_size	|| points[p].y < half_window_size
						|| (pyramid_old[LVL].h - points[p].y) < half_window_size) {
					printf("Input feature outside ROI %u, %u \n", points[p].x, points[p].y); //ADDED
					//CONC: consistent in not tracking edge features
					continue;
				}

				// Convert the point to a subpixel coordinate
				vectors[new_p].pos.x = (points[p].x * subpixel_factor) / exp; //this overflows for s_f = 1000; change point_t pos
				vectors[new_p].pos.y = (points[p].y * subpixel_factor) / exp; //this overflows for s_f = 1000; change point_t pos
				vectors[new_p].flow_x = 0;
				vectors[new_p].flow_y = 0;
				//printf("Convert point %u %u to subpix: %u, %u \n", points[p].x, points[p].y, vectors[new_p].pos.x,  vectors[new_p].pos.y);
			} else {
				// Convert last pyramid level flow into this pyramid level flow guess
				vectors[new_p].pos.x = 2 * vectors[new_p].pos.x;
				vectors[new_p].pos.y = 2 * vectors[new_p].pos.y;
				vectors[new_p].flow_x = 2 * vectors[new_p].flow_x;
				vectors[new_p].flow_y = 2 * vectors[new_p].flow_y;
			}

			// (1) determine the subpixel neighborhood in the old image
			image_subpixel_window(&pyramid_old[LVL], &window_I, &vectors[new_p].pos, subpixel_factor);

			// (2) get the x- and y- gradients
			image_gradients(&window_I, &window_DX, &window_DY);

			// (3) determine the 'G'-matrix [sum(Axx) sum(Axy); sum(Axy) sum(Ayy)], where sum is over the window
			int32_t G[4];
			image_calculate_g(&window_DX, &window_DY, G);

			// calculate G's determinant in subpixel units:
			int32_t Det = ((int64_t) G[0] * G[3] - G[1] * G[2])	/ subpixel_factor; // 1000 * 1000
			//printf("Max umnozak za det: %d \n", G[0]*G[3]); // milijuni
			//printf("Determinanta: %d \n", Det);

			// Check if the determinant is bigger than 1
			if (Det < 1) {
				printf("Determinant smaller than 1 for %d %d \n", points[p].x, points[p].y); //ADDED
				continue;
			}

			// (4) iterate over taking steps in the image to minimize the error:
			bool_t tracked = TRUE;
			for (uint8_t it = 0; it < max_iterations; it++) {
				struct point_t new_point = { vectors[new_p].pos.x  + vectors[new_p].flow_x,
											 vectors[new_p].pos.y + vectors[new_p].flow_y };
				// If the pixel is outside ROI, do not track it
				if (new_point.x / subpixel_factor < half_window_size || (pyramid_old[LVL].w - new_point.x / subpixel_factor) < half_window_size
						|| new_point.y / subpixel_factor < half_window_size || (pyramid_old[LVL].h - new_point.y / subpixel_factor)< half_window_size) {
					tracked = FALSE;
					printf("*New point outside ROI %u, %u \n", new_point.x,	new_point.y); //ADDED
					break;
				}

				//     [a] get the subpixel neighborhood in the new image
				image_subpixel_window(&pyramid_new[LVL], &window_J, &new_point, subpixel_factor);

				//     [b] determine the image difference between the two neighborhoods
				uint32_t error = image_difference(&window_I, &window_J, &window_diff);

				if (error > error_threshold && it > max_iterations / 2) {
					tracked = FALSE;
					printf("*Error larger than error treshold for %d %d \n", points[p].x, points[p].y); //ADDED
					break;
				}

				int32_t b_x = image_multiply(&window_diff, &window_DX, NULL) / 255;
				int32_t b_y = image_multiply(&window_diff, &window_DY, NULL) / 255;

				//     [d] calculate the additional flow step and possibly terminate the iteration
				int32_t step_x = (G[3] * b_x - G[1] * b_y) / Det; //CHANGED 16 -> 32
				int32_t step_y = (G[0] * b_y - G[2] * b_x) / Det; //CHANGED 16 -> 32
				//printf("step x %d step y %d \n", step_x, step_y);
				vectors[new_p].flow_x += step_x;
				vectors[new_p].flow_y += step_y;
				//printf("suma flow x %d  flow y %d \n",vectors[new_p].flow_x, vectors[new_p].flow_y);

				// Check if we exceeded the treshold
				if ((abs(step_x) + abs(step_y)) < step_threshold) {
					break;
				}
			}

			// If we tracked the point we update the index and the count
			if (tracked) {
				new_p++;
				(*points_cnt)++;
			}
		} // go through all points

	} // LVL of pyramid

	// Free the images
	image_free(&window_I);
	image_free(&window_J);
	image_free(&window_DX);
	image_free(&window_DY);
	image_free(&window_diff);

	for (uint8_t i = 0; i!= pyramid_level + 1; i++){
		image_free(&pyramid_old[i]);
		image_free(&pyramid_new[i]);
	}

	// Return the vectors
	return vectors;
}
