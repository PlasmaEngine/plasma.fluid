#include <FluidPlugin/FluidPluginPCH.h>

#include <Core/World/World.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <FluidPlugin/Components/FluidBodyComponent.h>
#include <RendererCore/AnimationSystem/Declarations.h>
#include <RendererCore/AnimationSystem/Skeleton.h>

namespace
{
  constexpr plUInt32 FluidBodyMaxShapes = 64;
  constexpr plUInt32 FluidBodyPoseMaxAge = 2; ///< Auto uses the skeleton while the last pose is at most this many updates old
} // namespace

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plFluidBodyComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Source", plFluidBodySource, m_Source),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.01f, 10.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("MinBoneLength", m_fMinBoneLength)->AddAttributes(new plDefaultValueAttribute(0.05f), new plClampValueAttribute(0.0f, 10.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("MaxShapes", m_uiMaxShapes)->AddAttributes(new plDefaultValueAttribute(24), new plClampValueAttribute(1, 64)),
    PL_MEMBER_PROPERTY("Strength", m_fStrength)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("Wake", m_fWake)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("Press", m_fPress)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("Obstacle", m_bObstacle),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgAnimationPoseUpdated, OnMsgAnimationPoseUpdated),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/Fluid"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plFluidBodyComponent::plFluidBodyComponent() = default;
plFluidBodyComponent::~plFluidBodyComponent() = default;

void plFluidBodyComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_Source;
  s << m_fRadius;
  s << m_fMinBoneLength;
  s << m_uiMaxShapes;
  s << m_fStrength;
  s << m_fWake;
  s << m_fPress;
  s << m_bObstacle;
}

void plFluidBodyComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s >> m_Source;
  s >> m_fRadius;
  s >> m_fMinBoneLength;
  s >> m_uiMaxShapes;
  s >> m_fStrength;
  s >> m_fWake;
  s >> m_fPress;
  s >> m_bObstacle;
}

void plFluidBodyComponent::OnActivated()
{
  SUPER::OnActivated();

  m_bHasHistory = false;
  m_LastCenters.Clear();
}

void plFluidBodyComponent::OnDeactivated()
{
  m_bHasPose = false;
  m_JointPositions.Clear();
  m_JointParents.Clear();
  m_bHasHistory = false;
  m_LastCenters.Clear();

  SUPER::OnDeactivated();
}

void plFluidBodyComponent::OnMsgAnimationPoseUpdated(plMsgAnimationPoseUpdated& ref_msg)
{
  if (ref_msg.m_pSkeleton == nullptr || ref_msg.m_pRootTransform == nullptr)
    return;

  const plUInt32 uiCount = plMath::Min<plUInt32>(ref_msg.m_pSkeleton->GetJointCount(), ref_msg.m_ModelTransforms.GetCount());

  m_JointPositions.SetCountUninitialized(uiCount);
  m_JointParents.SetCountUninitialized(uiCount);

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    plMat4 mFull;
    ref_msg.ComputeFullBoneTransform(i, mFull);
    m_JointPositions[i] = mFull.GetTranslationVector();
    m_JointParents[i] = ref_msg.m_pSkeleton->GetJointByIndex(static_cast<plUInt16>(i)).GetParentIndex();
  }

  m_uiPoseFrame = GetWorld()->GetUpdateCounter();
  m_bHasPose = uiCount > 0;
}

bool plFluidBodyComponent::UseSkeleton() const
{
  switch (m_Source.GetValue())
  {
    case plFluidBodySource::Bounds:
      return false;

    case plFluidBodySource::Skeleton:
      return m_bHasPose;

    default:
      return m_bHasPose && (GetWorld()->GetUpdateCounter() - m_uiPoseFrame) <= FluidBodyPoseMaxAge;
  }
}

plFluidBodySource::Enum plFluidBodyComponent::ComputeShapes(plDynamicArray<Shape>& inout_shapes, float fDeltaTime) const
{
  const plUInt32 uiFirst = inout_shapes.GetCount();

  plFluidBodySource::Enum source = plFluidBodySource::Bounds;
  if (UseSkeleton())
  {
    source = plFluidBodySource::Skeleton;
    ComputeSkeletonShapes(inout_shapes);
  }
  else if (m_Source != plFluidBodySource::Skeleton)
  {
    ComputeBoundsShape(inout_shapes);
  }

  const plUInt32 uiCount = inout_shapes.GetCount() - uiFirst;
  const bool bHasVelocity = m_bHasHistory && m_LastSource == source && m_LastCenters.GetCount() == uiCount && fDeltaTime > 0.0001f;

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    Shape& shape = inout_shapes[uiFirst + i];
    if (bHasVelocity)
    {
      shape.m_vLastCenter = m_LastCenters[i];
      shape.m_vVelocity = (shape.m_vCenter - shape.m_vLastCenter) / fDeltaTime;
    }
    else
    {
      shape.m_vLastCenter = shape.m_vCenter;
      shape.m_vVelocity = plVec3::MakeZero();
    }
  }

  return source;
}

void plFluidBodyComponent::AdvanceHistory(plArrayPtr<const Shape> shapes, plFluidBodySource::Enum source)
{
  m_LastCenters.SetCountUninitialized(shapes.GetCount());
  for (plUInt32 i = 0; i < shapes.GetCount(); ++i)
  {
    m_LastCenters[i] = shapes[i].m_vCenter;
  }

  m_LastSource = source;
  m_bHasHistory = true;
}

void plFluidBodyComponent::ComputeSkeletonShapes(plDynamicArray<Shape>& inout_shapes) const
{
  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();
  const float fRadius = plMath::Max(m_fRadius, 0.01f);
  const float fMinLength = plMath::Max(m_fMinBoneLength, 0.0f);
  const plUInt32 uiMaxShapes = plMath::Clamp<plUInt32>(m_uiMaxShapes, 1u, FluidBodyMaxShapes);

  struct Bone
  {
    plVec3 m_vParent;
    plVec3 m_vJoint;
    float m_fLength;
    plUInt32 m_uiJoint;
  };

  plHybridArray<Bone, 64> bones;
  for (plUInt32 i = 0; i < m_JointPositions.GetCount(); ++i)
  {
    const plUInt16 uiParent = m_JointParents[i];
    if (uiParent == plInvalidJointIndex || uiParent >= m_JointPositions.GetCount())
      continue;

    const plVec3 vParent = ownerTransform.TransformPosition(m_JointPositions[uiParent]);
    const plVec3 vJoint = ownerTransform.TransformPosition(m_JointPositions[i]);
    const float fLength = (vJoint - vParent).GetLength();
    if (fLength < fMinLength)
      continue;

    bones.PushBack({vParent, vJoint, fLength, i});
  }

  // The longest bones win; the survivors go back into joint order so a shape index names the same bone every frame.
  if (bones.GetCount() > uiMaxShapes)
  {
    bones.Sort([](const Bone& a, const Bone& b)
      { return a.m_fLength > b.m_fLength; });
    bones.SetCount(uiMaxShapes);
    bones.Sort([](const Bone& a, const Bone& b)
      { return a.m_uiJoint < b.m_uiJoint; });
  }

  for (const Bone& bone : bones)
  {
    Shape& shape = inout_shapes.ExpandAndGetRef();
    shape.m_vCenter = (bone.m_vParent + bone.m_vJoint) * 0.5f;
    shape.m_vAxis = (bone.m_vJoint - bone.m_vParent) * 0.5f;
    shape.m_vHalfExtents = plVec3(fRadius);
    shape.m_qRotation = plQuat::MakeIdentity();
    shape.m_fRadius = fRadius;
    shape.m_uiShape = static_cast<plUInt8>(plFluidEmitterShape::Capsule);
  }
}

void plFluidBodyComponent::ComputeBoundsShape(plDynamicArray<Shape>& inout_shapes) const
{
  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();
  const float fRadius = plMath::Max(m_fRadius, 0.01f);

  Shape& shape = inout_shapes.ExpandAndGetRef();
  shape.m_fRadius = fRadius;

  const plBoundingBoxSphere bounds = GetOwner()->GetLocalBounds();
  if (bounds.IsValid())
  {
    const plBoundingBox box = bounds.GetBox();
    const plVec3 vHalf = box.GetHalfExtents().CompMul(ownerTransform.m_vScale.Abs());

    if (vHalf.x > 0.001f || vHalf.y > 0.001f || vHalf.z > 0.001f)
    {
      shape.m_vCenter = ownerTransform.TransformPosition(box.GetCenter());
      shape.m_vHalfExtents = vHalf;
      shape.m_vAxis = ownerTransform.m_qRotation * plVec3::MakeAxisX();
      shape.m_qRotation = ownerTransform.m_qRotation;
      shape.m_uiShape = static_cast<plUInt8>(plFluidEmitterShape::Box);
      return;
    }
  }

  // No usable bounds: a sphere of Radius at the owner.
  shape.m_vCenter = ownerTransform.m_vPosition;
  shape.m_vHalfExtents = plVec3(fRadius);
  shape.m_vAxis = plVec3::MakeZero();
  shape.m_qRotation = plQuat::MakeIdentity();
  shape.m_uiShape = static_cast<plUInt8>(plFluidEmitterShape::Sphere);
}

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_Components_FluidBodyComponent);
