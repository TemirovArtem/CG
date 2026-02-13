//***************************************************************************************
// Camera.h by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

#include "Camera.h"

using namespace DirectX;

Camera::Camera()
{
	// Increased near plane for better terrain visibility at steep angles
	SetLens(0.25f*MathHelper::Pi, 1.0f, 5.0f, 15000.0f);  // Near: 5m, Far: 15000m
}

Camera::~Camera()
{
}

XMVECTOR Camera::GetPosition()const
{
	return XMLoadFloat3(&mPosition);
}

XMFLOAT3 Camera::GetPosition3f()const
{
	return mPosition;
}

void Camera::SetPosition(float x, float y, float z)
{
	mPosition = XMFLOAT3(x, y, z);
	mViewDirty = true;
}

void Camera::SetPosition(const XMFLOAT3& v)
{
	mPosition = v;
	mViewDirty = true;
}

XMVECTOR Camera::GetRight()const
{
	return XMLoadFloat3(&mRight);
}

XMFLOAT3 Camera::GetRight3f()const
{
	return mRight;
}

XMVECTOR Camera::GetUp()const
{
	return XMLoadFloat3(&mUp);
}

XMFLOAT3 Camera::GetUp3f()const
{
	return mUp;
}

XMVECTOR Camera::GetLook()const
{
	return XMLoadFloat3(&mLook);
}

XMFLOAT3 Camera::GetLook3f()const
{
	return mLook;
}

float Camera::GetNearZ()const
{
	return mNearZ;
}

float Camera::GetFarZ()const
{
	return mFarZ;
}

float Camera::GetAspect()const
{
	return mAspect;
}

float Camera::GetFovY()const
{
	return mFovY;
}

float Camera::GetFovX()const
{
	float halfWidth = 0.5f*GetNearWindowWidth();
	return 2.0f*atan(halfWidth / mNearZ);
}

float Camera::GetNearWindowWidth()const
{
	return mAspect * mNearWindowHeight;
}

float Camera::GetNearWindowHeight()const
{
	return mNearWindowHeight;
}

float Camera::GetFarWindowWidth()const
{
	return mAspect * mFarWindowHeight;
}

float Camera::GetFarWindowHeight()const
{
	return mFarWindowHeight;
}

void Camera::SetLens(float fovY, float aspect, float zn, float zf)
{
	// cache properties
	mFovY = fovY;
	mAspect = aspect;
	mNearZ = zn;
	mFarZ = zf;

	mNearWindowHeight = 2.0f * mNearZ * tanf( 0.5f*mFovY );
	mFarWindowHeight  = 2.0f * mFarZ * tanf( 0.5f*mFovY );

	XMMATRIX P = XMMatrixPerspectiveFovLH(mFovY, mAspect, mNearZ, mFarZ);
	XMStoreFloat4x4(&mProj, P);
}

void Camera::LookAt(FXMVECTOR pos, FXMVECTOR target, FXMVECTOR worldUp)
{
	XMVECTOR L = XMVector3Normalize(XMVectorSubtract(target, pos));
	XMVECTOR R = XMVector3Normalize(XMVector3Cross(worldUp, L));
	XMVECTOR U = XMVector3Cross(L, R);

	XMStoreFloat3(&mPosition, pos);
	XMStoreFloat3(&mLook, L);
	XMStoreFloat3(&mRight, R);
	XMStoreFloat3(&mUp, U);

	mViewDirty = true;
}

void Camera::LookAt(const XMFLOAT3& pos, const XMFLOAT3& target, const XMFLOAT3& up)
{
	XMVECTOR P = XMLoadFloat3(&pos);
	XMVECTOR T = XMLoadFloat3(&target);
	XMVECTOR U = XMLoadFloat3(&up);

	LookAt(P, T, U);

	mViewDirty = true;
}

XMMATRIX Camera::GetView()const
{
	assert(!mViewDirty);
	return XMLoadFloat4x4(&mView);
}

XMMATRIX Camera::GetProj()const
{
	return XMLoadFloat4x4(&mProj);
}


XMFLOAT4X4 Camera::GetView4x4f()const
{
	assert(!mViewDirty);
	return mView;
}

XMFLOAT4X4 Camera::GetProj4x4f()const
{
	return mProj;
}

void Camera::Strafe(float d)
{
	// mPosition += d*mRight
	XMVECTOR s = XMVectorReplicate(d);
	XMVECTOR r = XMLoadFloat3(&mRight);
	XMVECTOR p = XMLoadFloat3(&mPosition);
	XMStoreFloat3(&mPosition, XMVectorMultiplyAdd(s, r, p));

	mViewDirty = true;
}

void Camera::Walk(float d)
{
	// mPosition += d*mLook
	XMVECTOR s = XMVectorReplicate(d);
	XMVECTOR l = XMLoadFloat3(&mLook);
	XMVECTOR p = XMLoadFloat3(&mPosition);
	XMStoreFloat3(&mPosition, XMVectorMultiplyAdd(s, l, p));

	mViewDirty = true;
}

void Camera::Pitch(float angle)
{
	// Rotate up and look vector about the right vector.

	XMMATRIX R = XMMatrixRotationAxis(XMLoadFloat3(&mRight), angle);

	XMStoreFloat3(&mUp,   XMVector3TransformNormal(XMLoadFloat3(&mUp), R));
	XMStoreFloat3(&mLook, XMVector3TransformNormal(XMLoadFloat3(&mLook), R));

	mViewDirty = true;
}

void Camera::RotateY(float angle)
{
	// Rotate the basis vectors about the world y-axis.

	XMMATRIX R = XMMatrixRotationY(angle);

	XMStoreFloat3(&mRight,   XMVector3TransformNormal(XMLoadFloat3(&mRight), R));
	XMStoreFloat3(&mUp, XMVector3TransformNormal(XMLoadFloat3(&mUp), R));
	XMStoreFloat3(&mLook, XMVector3TransformNormal(XMLoadFloat3(&mLook), R));

	mViewDirty = true;
}

void Camera::SetYaw(float yaw)
{
	mYaw = yaw;
	mViewDirty = true;
}

void Camera::SetPitch(float pitch)
{
	// Clamp pitch to prevent gimbal lock
	const float maxPitch = 1.55f; // About 89 degrees
	mPitch = MathHelper::Clamp(pitch, -maxPitch, maxPitch);
	mViewDirty = true;
}

float Camera::GetYaw() const
{
	return mYaw;
}

float Camera::GetPitch() const
{
	return mPitch;
}

void Camera::UpdateFromYawPitch()
{
	// Calculate look direction from yaw and pitch
	float cosYaw = cosf(mYaw);
	float sinYaw = sinf(mYaw);
	float cosPitch = cosf(mPitch);
	float sinPitch = sinf(mPitch);

	// Look vector (forward direction)
	mLook.x = cosYaw * cosPitch;
	mLook.y = sinPitch;
	mLook.z = sinYaw * cosPitch;

	// Right vector (cross product of world up and look)
	XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMVECTOR look = XMLoadFloat3(&mLook);
	XMVECTOR right = XMVector3Cross(worldUp, look);
	right = XMVector3Normalize(right);
	XMStoreFloat3(&mRight, right);

	// Up vector (cross product of look and right)
	XMVECTOR up = XMVector3Cross(look, right);
	XMStoreFloat3(&mUp, up);

	mViewDirty = true;
}

void Camera::UpdateViewMatrix()
{
	if(mViewDirty)
	{
		XMVECTOR R = XMLoadFloat3(&mRight);
		XMVECTOR U = XMLoadFloat3(&mUp);
		XMVECTOR L = XMLoadFloat3(&mLook);
		XMVECTOR P = XMLoadFloat3(&mPosition);

		// Keep camera's axes orthogonal to each other and of unit length.
		L = XMVector3Normalize(L);
		U = XMVector3Normalize(XMVector3Cross(L, R));

		// U, L already ortho-normal, so no need to normalize cross product.
		R = XMVector3Cross(U, L);

		// Fill in the view matrix entries.
		float x = -XMVectorGetX(XMVector3Dot(P, R));
		float y = -XMVectorGetX(XMVector3Dot(P, U));
		float z = -XMVectorGetX(XMVector3Dot(P, L));

		XMStoreFloat3(&mRight, R);
		XMStoreFloat3(&mUp, U);
		XMStoreFloat3(&mLook, L);

		mView(0, 0) = mRight.x;
		mView(1, 0) = mRight.y;
		mView(2, 0) = mRight.z;
		mView(3, 0) = x;

		mView(0, 1) = mUp.x;
		mView(1, 1) = mUp.y;
		mView(2, 1) = mUp.z;
		mView(3, 1) = y;

		mView(0, 2) = mLook.x;
		mView(1, 2) = mLook.y;
		mView(2, 2) = mLook.z;
		mView(3, 2) = z;

		mView(0, 3) = 0.0f;
		mView(1, 3) = 0.0f;
		mView(2, 3) = 0.0f;
		mView(3, 3) = 1.0f;

		mViewDirty = false;
	}
}

DirectX::BoundingFrustum Camera::CreateFrustum() const
{
	// Create frustum from projection matrix, then transform to world space by inverse view
	DirectX::BoundingFrustum frustum;
	DirectX::XMMATRIX view = GetView();
	DirectX::XMMATRIX proj = GetProj();

	DirectX::BoundingFrustum::CreateFromMatrix(frustum, proj);

	DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);
	frustum.Transform(frustum, invView);

	return frustum;
}

DirectX::BoundingFrustum Camera::CreateFrustumFromMatrix(const DirectX::XMMATRIX& viewProj) const
{
	DirectX::BoundingFrustum frustum;

	// viewProj = view * proj; derive proj using current view
	DirectX::XMMATRIX view = GetView();
	DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);
	DirectX::XMMATRIX proj = DirectX::XMMatrixMultiply(invView, viewProj);

	DirectX::BoundingFrustum::CreateFromMatrix(frustum, proj);
	frustum.Transform(frustum, invView);
	return frustum;
}

DirectX::BoundingFrustum Camera::CreateFrustumWithFovScale(float fovScale) const
{
	// clamp
	fovScale = fovScale <= 0.01f ? 0.01f : fovScale;
	fovScale = fovScale >= 1.00f ? 1.00f : fovScale;

	DirectX::XMMATRIX view = GetView();
	float fovY = GetFovY();
	float aspect = GetAspect();
	float zn = GetNearZ();
	float zf = GetFarZ();

	float newFovY = fovY * fovScale; // меняем фову
	DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(newFovY, aspect, zn, zf); // делаем новую матрицу проекции с другим фовом

	DirectX::BoundingFrustum frustum; // создаем новый фрустум как и обычно
	DirectX::BoundingFrustum::CreateFromMatrix(frustum, proj);

	DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view); // инвертируем матрицу вида
	frustum.Transform(frustum, invView);
	return frustum;
}


