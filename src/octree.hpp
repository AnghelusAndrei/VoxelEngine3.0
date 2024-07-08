#pragma once

#include <stack>
#include <functional>
#include <cstdlib>

#include "renderer.hpp"


#define maxDepth 16

class Octree{
    public:
        struct NodeBase {
            unsigned isNode : 1;
        };

        struct NodeData {
            unsigned isNode : 1;
            unsigned count : 3;
            unsigned next : 28;
        };

        struct LeafData {
            unsigned isNode : 1;
            unsigned material : 7;
            unsigned normal : 24;
        };

        union Node {
            NodeBase base;
            NodeData node;
            LeafData leaf;
            uint32_t raw;
        };

        std::vector<Node> data;
    public:
        struct Config{
            uint8_t depth;
            Renderer *renderer;
        };

        Octree(Config *config);
        ~Octree();
        void Update();
        void Debug(Log *log);

        uint32_t lookup(glm::uvec3 position);
        void insert(glm::uvec3 position, Node leaf);
        void remove(glm::uvec3 position);

        static uint32_t packedNormal(glm::vec3& normal);
        GLuint gl_ID;
        GLuint texBufferID;
        GLuint depthUniformLocation;

        uint8_t depth;
        uint32_t capacity;
    private:
        Renderer *renderer;
        uint32_t newNode = 8;
        std::stack<uint32_t> freeNodes;
        

        void UpdateNode(uint32_t index);

        uint32_t utils_p2r[maxDepth];
        uint32_t locate(glm::uvec3 position, uint32_t depth_);
        bool contained(glm::uvec3 position1, glm::uvec3 position2, uint32_t depth_);
};