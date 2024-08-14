#pragma once

#include "widget.hpp"

class ViewportWidget : public Widget{
    public:
        ViewportWidget(const char* name_, glm::vec2 size_, glm::ivec2 fbsize_, float aspect_ratio_);
        void Draw() override;
        bool getDrawID(GLuint gl_ID_);

        ImVec2 avail_size;

    private:
        GLuint gl_ID;
        glm::vec2 size;
        glm::ivec2 fbsize;
        float aspect_ratio;
};