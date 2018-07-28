/*
 * Copyright 2011-2018 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __LIGHT_TREE_H__
#define __LIGHT_TREE_H__

#include "util/util_boundbox.h"
#include "util/util_vector.h"

CCL_NAMESPACE_BEGIN

class Light;
class Object;
class Scene;

#define LIGHT_BVH_NODE_SIZE 4

struct Orientation{ // Orientation bounds
	Orientation(){
		axis = make_float3(0.0f,0.0f,0.0f);
		theta_o = 0;
		theta_e = 0;
	}

	Orientation(const float3& a, float o, float e): axis(a), theta_o(o), theta_e(e) {}

	float3 axis;
	float theta_o;
	float theta_e;
};

/* Temporary data structure for nodes during build */
struct BVHBuildNode {

	BVHBuildNode() {
		children[0] = children[1] = NULL;
		bbox = BoundBox::empty;
	}

	void init_leaf(unsigned int first, uint n, const BoundBox& b,
	               const Orientation &c, double e, double e_var){
		firstPrimOffset = first;
		num_emitters = n;
		bbox = b;
		bcone = c;
		energy = (float)e;
		energy_variance = (float)e_var;
		is_leaf = true;
	}

	void init_interior(unsigned int axis, BVHBuildNode *c0, BVHBuildNode *c1,
	                   const Orientation &c, uint n, double e, double e_var){
		children[0] = c0;
		children[1] = c1;
		splitAxis = axis;
		bbox = merge(c0->bbox, c1->bbox);
		num_emitters = n;
		bcone = c;
		energy = (float)e;
		energy_variance = (float)e_var;
		is_leaf = false;
	}

	Orientation bcone;
	BoundBox bbox;
	BVHBuildNode *children[2];
	unsigned int splitAxis, firstPrimOffset, num_emitters;
	float energy, energy_variance;
	bool is_leaf;
};

struct BVHPrimitiveInfo {
	BVHPrimitiveInfo() {
		bbox = BoundBox::empty;
	}
	BVHPrimitiveInfo(unsigned int primitiveNumber, const BoundBox &bounds,
	                 const Orientation& oBounds, float e)
	    : primitiveNumber(primitiveNumber),
	      bbox(bounds),
	      centroid(bounds.center()),
	      energy(e)
	{
		bcone.axis = oBounds.axis;
		bcone.theta_o = oBounds.theta_o;
		bcone.theta_e = oBounds.theta_e;
	}
	unsigned int primitiveNumber;
	BoundBox bbox;
	float3 centroid;
	float energy;
	Orientation bcone;
};

struct Primitive {
	/* If prim_id >= 0 then the primitive is a triangle and prim_id is a global
	 * triangle index.
	 * If prim_id < 0 then the primitive is a lamp and -prim_id-1 is an index
	 * into the klights array on the device. */
	int prim_id;
	union {
		/* which object the triangle belongs to */
		int object_id;
		/* index for this lamp in the scene->lights array */
		int lamp_id;
	};
	Primitive(int prim, int object): prim_id(prim), object_id(object) {}
};

struct CompareToBucket {
	CompareToBucket(int split, int num, int d, const BoundBox &b):
	    centroidBbox(b)
	{
		splitBucket = split;
		nBuckets = num;
		dim = d;
		invExtent = 1.0f / (centroidBbox.max[dim] - centroidBbox.min[dim]);
	}

	bool operator() (const BVHPrimitiveInfo &p) const {
		int bucket_id = (int)((float)nBuckets * (p.centroid[dim]-centroidBbox.min[dim]) *
		                      invExtent);
		if (bucket_id == nBuckets) {
			bucket_id = nBuckets - 1;
		}

		return bucket_id <= splitBucket;
	}

	int splitBucket, nBuckets, dim;
	float invExtent;
	const BoundBox &centroidBbox;
};

// TODO: Have this struct in kernel_types.h instead?
struct CompactNode {
	CompactNode():
	    energy(0.0f), energy_variance(0.0f), secondChildOffset(-1), prim_id(-1),
	    num_emitters(-1), bounds_w(BoundBox::empty)
	{
		bounds_o.axis = make_float3(0.0f);
		bounds_o.theta_o = 0.0f;
		bounds_o.theta_e = 0.0f;
	}

	float energy;
	float energy_variance;
	int secondChildOffset; // only for interior
	int prim_id;   // Index into the primitives array (only for leaf)
	int num_emitters;

	BoundBox bounds_w; // World space bounds
	Orientation bounds_o; // Orientation bounds

};

class LightTree
{
public:
	LightTree(const vector<Primitive>& prims_,
	          const vector<Object*>& objects_,
	          const vector<Light*>& lights_,
	          Scene* scene,
	          const unsigned int maxPrimsInNode_);

	const vector<Primitive>& get_primitives() const {
		return primitives;
	}

	const vector<CompactNode>& get_nodes() const {
		return nodes;
	}

	BoundBox get_bbox(const Primitive& prim);
	Orientation get_bcone(const Primitive& prim);
	float get_energy(const Primitive &prim);

private:

	BVHBuildNode* recursive_build(const unsigned int start,
	                              const unsigned int end,
	                              vector<BVHPrimitiveInfo> &buildData,
	                              unsigned int &totalNodes,
	                              vector<Primitive> &orderedPrims);

	Orientation aggregate_bounding_cones(const vector<Orientation> &bcones);
	Orientation cone_union(const Orientation& a, const Orientation& b);
	float calculate_cone_measure(const Orientation &bcone);
	int flattenBVHTree(const BVHBuildNode &node, int &offset);
	void split_saoh(const BoundBox &centroidBbox,
	                const vector<BVHPrimitiveInfo> &buildData,
	                const int start, const int end, const int nBuckets,
	                const float node_energy, const float node_M_Omega,
	                const BoundBox &node_bbox,
	                float &min_cost, int &min_dim, int &min_bucket);

	// Stores an index for each emissive primitive, if < 0 then lamp
	// To be able to find which triangle the id refers to I also need to
	// know which object it came from.
	vector<Primitive> primitives; // create device_vector<KernelLightDistribution> out of this one?
	vector<Object*> objects;
	vector<Light*> lights;
	unsigned int maxPrimsInNode;
	Scene *scene;

	vector<CompactNode> nodes;
};

CCL_NAMESPACE_END

#endif // __LIGHT_TREE_H__