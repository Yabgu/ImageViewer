module;

#include <GLFW/glfw3.h>
#include <functional>

export module HotkeysHandler;

export class HotkeysHandler {
public:
    using ZoomCallback = std::function<void(int)>;
    using PanCallback = std::function<void(int, int)>;
    using SetZoomCallback = std::function<void(double)>;
    using FitToWindowCallback = std::function<void()>;

    void setZoomCallback(ZoomCallback callback) {
        zoomCallbackMember = std::move(callback);
    }

    void setPanCallback(PanCallback callback) {
        panCallbackMember = std::move(callback);
    }

    void setSetZoomCallback(SetZoomCallback callback) {
        setZoomCallbackMember = std::move(callback);
    }

    void setFitToWindowCallback(FitToWindowCallback callback) {
        fitToWindowCallbackMember = std::move(callback);
    }

    void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            switch (key) {
                case GLFW_KEY_KP_ADD: // Numpad +
                case GLFW_KEY_EQUAL: // Plus key
                    if (zoomCallbackMember) zoomCallbackMember(10); // Zoom in
                    break;
                case GLFW_KEY_KP_SUBTRACT: // Numpad -
                case GLFW_KEY_MINUS: // Minus key
                    if (zoomCallbackMember) zoomCallbackMember(-10); // Zoom out
                    break;
                case GLFW_KEY_UP: // Arrow up
                    if (panCallbackMember) panCallbackMember(0, 10); // Pan up
                    break;
                case GLFW_KEY_DOWN: // Arrow down
                    if (panCallbackMember) panCallbackMember(0, -10); // Pan down
                    break;
                case GLFW_KEY_LEFT: // Arrow left
                    if (panCallbackMember) panCallbackMember(10, 0); // Pan left
                    break;
                case GLFW_KEY_RIGHT: // Arrow right
                    if (panCallbackMember) panCallbackMember(-10, 0); // Pan right
                    break;
                case GLFW_KEY_KP_MULTIPLY: // * key
                    if (setZoomCallbackMember) setZoomCallbackMember(1.0); // Zoom 1:1
                    break;
                case GLFW_KEY_KP_DIVIDE: // / key
                    if (fitToWindowCallbackMember) fitToWindowCallbackMember(); // Fit to window
                    break;
                default:
                    break;
            }
        }
    }

private:
    ZoomCallback zoomCallbackMember;
    PanCallback panCallbackMember;
    SetZoomCallback setZoomCallbackMember;
    FitToWindowCallback fitToWindowCallbackMember;
};
