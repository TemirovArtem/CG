//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "stdafx.h"
#include "SimpleCamera.h"

SimpleCamera::SimpleCamera():
    m_initialPosition(0, 0, 0),
    m_position(m_initialPosition),
    m_yaw(XM_PI),
    m_pitch(0.0f),
    m_lookDirection(0, 0, -1),
    m_upDirection(0, 1, 0),
    m_moveSpeed(20.0f),
    m_turnSpeed(XM_PIDIV2),
    m_keysPressed{},
    m_ctrlPressed(false),
    m_lastMouseX(0),
    m_lastMouseY(0),
    m_mouseLeftButtonDown(false),
    m_mouseCaptured(false)
{
}

void SimpleCamera::Init(XMFLOAT3 position)
{
    m_initialPosition = position;
    Reset();
}

void SimpleCamera::SetMoveSpeed(float unitsPerSecond)
{
    m_moveSpeed = unitsPerSecond;
}

void SimpleCamera::SetTurnSpeed(float radiansPerSecond)
{
    m_turnSpeed = radiansPerSecond;
}

void SimpleCamera::Reset()
{
    m_position = m_initialPosition;
    m_yaw = XM_PI;
    m_pitch = 0.0f;
    m_lookDirection = { 0, 0, -1 };
}

void SimpleCamera::Update(float elapsedSeconds)
{
    float moveInterval = m_moveSpeed * elapsedSeconds;
    float rotateInterval = m_turnSpeed * elapsedSeconds;

    // Calculate the move vector in camera space (relative to look direction).
    XMFLOAT3 move(0, 0, 0);

    if (m_keysPressed.w)
    {
        move.x += m_lookDirection.x;
        move.y += m_lookDirection.y;
        move.z += m_lookDirection.z;
    }
    if (m_keysPressed.s)
    {
        move.x -= m_lookDirection.x;
        move.y -= m_lookDirection.y;
        move.z -= m_lookDirection.z;
    }

    // Strafe movement (perpendicular to look direction, in XZ plane)
    XMFLOAT3 rightDir;
    rightDir.x = m_lookDirection.z;
    rightDir.y = 0;
    rightDir.z = -m_lookDirection.x;
    float rightLen = sqrtf(rightDir.x * rightDir.x + rightDir.z * rightDir.z);
    if (rightLen > 0.0001f)
    {
        rightDir.x /= rightLen;
        rightDir.z /= rightLen;
    }

    if (m_keysPressed.a)
    {
        move.x += rightDir.x;
        move.z += rightDir.z;
    }
    if (m_keysPressed.d)
    {
        move.x -= rightDir.x;
        move.z -= rightDir.z;
    }

    // Vertical movement: Ctrl+Space = up, Space = down
    if (m_keysPressed.up)   // This is Ctrl+Space
    {
        move.y -= 1.0f;
    }
    if (m_keysPressed.down) // This is Space
    {
        move.y += 1.0f;
    }

    // Normalize if moving diagonally
    float moveLen = sqrtf(move.x * move.x + move.y * move.y + move.z * move.z);
    if (moveLen > 0.1f)
    {
        move.x /= moveLen;
        move.y /= moveLen;
        move.z /= moveLen;
    }

    // Apply movement
    m_position.x += move.x * moveInterval;
    m_position.y += move.y * moveInterval;
    m_position.z += move.z * moveInterval;

    // Determine the look direction based on yaw and pitch.
    float r = cosf(m_pitch);
    m_lookDirection.x = r * sinf(m_yaw);
    m_lookDirection.y = sinf(m_pitch);
    m_lookDirection.z = r * cosf(m_yaw);
}

XMMATRIX SimpleCamera::GetViewMatrix()
{
    return XMMatrixLookToRH(XMLoadFloat3(&m_position), XMLoadFloat3(&m_lookDirection), XMLoadFloat3(&m_upDirection));
}

XMMATRIX SimpleCamera::GetProjectionMatrix(float fov, float aspectRatio, float nearPlane, float farPlane)
{
    return XMMatrixPerspectiveFovRH(fov, aspectRatio, nearPlane, farPlane);
}

void SimpleCamera::OnKeyDown(WPARAM key)
{
    switch (key)
    {
    case 'W':
        m_keysPressed.w = true;
        break;
    case 'A':
        m_keysPressed.a = true;
        break;
    case 'S':
        m_keysPressed.s = true;
        break;
    case 'D':
        m_keysPressed.d = true;
        break;
    case VK_CONTROL:
        m_ctrlPressed = true;
        break;
    case VK_SPACE:
        if (m_ctrlPressed)
            m_keysPressed.up = true;    // Ctrl+Space = move up
        else
            m_keysPressed.down = true;   // Space = move down
        break;
    case VK_LEFT:
        m_keysPressed.left = true;
        break;
    case VK_RIGHT:
        m_keysPressed.right = true;
        break;
    case VK_UP:
        m_keysPressed.up = true;
        break;
    case VK_DOWN:
        m_keysPressed.down = true;
        break;
    case VK_ESCAPE:
        Reset();
        break;
    }
}

void SimpleCamera::OnKeyUp(WPARAM key)
{
    switch (key)
    {
    case 'W':
        m_keysPressed.w = false;
        break;
    case 'A':
        m_keysPressed.a = false;
        break;
    case 'S':
        m_keysPressed.s = false;
        break;
    case 'D':
        m_keysPressed.d = false;
        break;
    case VK_CONTROL:
        m_ctrlPressed = false;
        break;
    case VK_SPACE:
        m_keysPressed.up = false;
        m_keysPressed.down = false;
        break;
    case VK_LEFT:
        m_keysPressed.left = false;
        break;
    case VK_RIGHT:
        m_keysPressed.right = false;
        break;
    case VK_UP:
        m_keysPressed.up = false;
        break;
    case VK_DOWN:
        m_keysPressed.down = false;
        break;
    }
}

void SimpleCamera::OnMouseMove(int x, int y, bool leftButtonDown)
{
    m_mouseLeftButtonDown = leftButtonDown;

    if (m_mouseLeftButtonDown)
    {
        int deltaX = x - m_lastMouseX;
        int deltaY = y - m_lastMouseY;

        // Sensitivity factor - adjust as needed
        float sensitivity = 0.005f;

        m_yaw -= deltaX * sensitivity;
        m_pitch -= deltaY * sensitivity;

        // Clamp pitch to prevent flipping
        m_pitch = min(m_pitch, XM_PI * 0.49f);
        m_pitch = max(-XM_PI * 0.49f, m_pitch);
    }

    m_lastMouseX = x;
    m_lastMouseY = y;
}

void SimpleCamera::OnMouseDown(int button, int x, int y)
{
    m_lastMouseX = x;
    m_lastMouseY = y;
    m_mouseLeftButtonDown = true;
}

void SimpleCamera::OnMouseUp(int button, int x, int y)
{
    m_mouseLeftButtonDown = false;
}
