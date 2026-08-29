////////////////////////////////////////////////////////////////////////////
//	Module 		: physic_item.cpp
//	Created 	: 11.02.2004
//  Modified 	: 11.02.2004
//	Author		: Dmitriy Iassenev
//	Description : Physic item
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "physic_item.h"
#include "physicsshell.h"
#include "xrserver_objects.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"

#define CHOOSE_MAX(x, inst_x, y, inst_y, z, inst_z) \
    if (x > y) \
        if (x > z) \
        { \
            inst_x; \
        } \
        else \
        { \
            inst_z; \
        } \
    else if (y > z) \
    { \
        inst_y; \
    } \
    else \
    { \
        inst_z; \
    }

CPhysicItem::CPhysicItem() { init(); }

CPhysicItem::~CPhysicItem() { xr_delete(m_pPhysicsShell); }

void CPhysicItem::init() { m_pPhysicsShell = 0; }

void CPhysicItem::reinit()
{
    inherited::reinit();
    m_ready_to_destroy = false;
}

void CPhysicItem::Load(LPCSTR section)
{
    inherited::Load(section);

    m_use_hud_model_as_world = READ_IF_EXISTS(pSettings, r_bool, section, "use_hud_model_as_world", false);
    m_auto_generate_collision = READ_IF_EXISTS(pSettings, r_bool, section, "auto_generate_collision", true);
    m_force_auto_generated_collision = READ_IF_EXISTS(
        pSettings, r_bool, section, "force_auto_generated_collision", m_use_hud_model_as_world);
    m_auto_collision_max_boxes = READ_IF_EXISTS(pSettings, r_u32, section, "auto_collision_max_boxes", 16);
    clamp(m_auto_collision_max_boxes, 1u, 64u);

    m_world_model_scale = READ_IF_EXISTS(pSettings, r_float, section, "world_scaling", 1.f);

    if (m_use_hud_model_as_world)
    {
        if (pSettings->line_exist(section, "hud_world_visual"))
            m_hud_world_visual = pSettings->r_string(section, "hud_world_visual");
        else if (pSettings->line_exist(section, "hud"))
        {
            LPCSTR hud_section = pSettings->r_string(section, "hud");
            if (pSettings->section_exist(hud_section))
            {
                if (pSettings->line_exist(hud_section, "item_visual"))
                    m_hud_world_visual = pSettings->r_string(hud_section, "item_visual");
                else if (pSettings->line_exist(hud_section, "visual"))
                    m_hud_world_visual = pSettings->r_string(hud_section, "visual");

                m_world_model_scale = READ_IF_EXISTS(pSettings, r_float, hud_section, "world_scale", m_world_model_scale);
            }
        }

        m_world_model_scale = READ_IF_EXISTS(pSettings, r_float, section, "hud_world_scale", m_world_model_scale);
        if (!m_hud_world_visual.c_str() || !m_hud_world_visual.size())
        {
            Msg("! [%s]: use_hud_model_as_world is enabled, but neither hud_world_visual nor HUD item_visual exists", section);
            m_use_hud_model_as_world = false;
            m_force_auto_generated_collision = READ_IF_EXISTS(pSettings, r_bool, section, "force_auto_generated_collision", false);
        }
    }

    clamp(m_world_model_scale, 0.001f, 100.f);
}

void CPhysicItem::reload(LPCSTR section) { inherited::reload(section); }

void CPhysicItem::OnH_B_Independent(bool just_before_destroy)
{
    inherited::OnH_B_Independent(just_before_destroy);

    if (m_ready_to_destroy)
        return;

    setVisible(TRUE);
    setEnabled(TRUE);

    if (!just_before_destroy)
        activate_physic_shell();
}

void CPhysicItem::OnH_B_Chield()
{
    inherited::OnH_B_Chield();

    setVisible(FALSE);
    setEnabled(FALSE);

    inherited::deactivate_physics_shell();
}

BOOL CPhysicItem::net_Spawn(CSE_Abstract* DC)
{
    if (!inherited::net_Spawn(DC))
        return (FALSE);

    ApplyHudWorldVisual();

    smart_cast<IKinematics*>(Visual())->CalculateBones_Invalidate();
    smart_cast<IKinematics*>(Visual())->CalculateBones();
    CSE_Abstract* abstract = (CSE_Abstract*)DC;
    if (0xffff == abstract->ID_Parent)
    {
        if (!PPhysicsShell())
            setup_physic_shell();
        // else processing_deactivate();//.
    }

    setVisible(TRUE);
    setEnabled(TRUE);

    return (TRUE);
}

void CPhysicItem::net_Destroy() { inherited::net_Destroy(); }

void CPhysicItem::UpdateCL()
{
    //	if (!xr_strcmp("bolt",cName()))
    //		Log					("--- B - CBolt",renderable.xform);
    if (!H_Parent() && m_pPhysicsShell && m_pPhysicsShell->isActive())
        m_pPhysicsShell->InterpolateGlobalTransform(&XFORM());
    //	if (!xr_strcmp("bolt",cName()))
    //		Log						("--- C - CBolt",renderable.xform);
    inherited::UpdateCL();
    //	if (!xr_strcmp("bolt",cName()))
    //		Log						("--- D - CBolt",renderable.xform);
}

void CPhysicItem::activate_physic_shell()
{
    CObject* object = smart_cast<CObject*>(H_Parent());
    R_ASSERT(object);
    XFORM().set(object->XFORM());
    inherited::activate_physic_shell();
    IKinematics* K = smart_cast<IKinematics*>(Visual());
    if (K)
    {
        K->CalculateBones_Invalidate();
        K->CalculateBones();
    }
    /// m_pPhysicsShell->Update		();
}

void CPhysicItem::setup_physic_shell()
{
    inherited::setup_physic_shell();
    IKinematics* K = smart_cast<IKinematics*>(Visual());
    if (K)
    {
        K->CalculateBones_Invalidate();
        K->CalculateBones();
    }
    // m_pPhysicsShell->Update		();
}

void CPhysicItem::create_box_physic_shell()
{
    // Physics (Box)
    Fobb obb;
    Visual()->getVisData().box.get_CD(obb.m_translate, obb.m_halfsize);
    obb.m_rotate.identity();

    // Physics (Elements)
    CPhysicsElement* E = P_create_Element();
    R_ASSERT(E);
    E->add_Box(obb);
    // Physics (Shell)
    m_pPhysicsShell = P_create_Shell();
    R_ASSERT(m_pPhysicsShell);
    m_pPhysicsShell->add_Element(E);
    m_pPhysicsShell->setDensity(2000.f);
}

void CPhysicItem::create_box2sphere_physic_shell()
{
    // Physics (Box)
    Fobb obb;
    Visual()->getVisData().box.get_CD(obb.m_translate, obb.m_halfsize);
    obb.m_rotate.identity();

    // Physics (Elements)
    CPhysicsElement* E = P_create_Element();
    R_ASSERT(E);

    Fvector ax;
    float radius;
    CHOOSE_MAX(obb.m_halfsize.x, ax.set(obb.m_rotate.i); ax.mul(obb.m_halfsize.x); radius = _min(obb.m_halfsize.y, obb.m_halfsize.z); obb.m_halfsize.y /= 2.f;
               obb.m_halfsize.z /= 2.f, obb.m_halfsize.y, ax.set(obb.m_rotate.j); ax.mul(obb.m_halfsize.y); radius = _min(obb.m_halfsize.x, obb.m_halfsize.z);
               obb.m_halfsize.x /= 2.f; obb.m_halfsize.z /= 2.f, obb.m_halfsize.z, ax.set(obb.m_rotate.k); ax.mul(obb.m_halfsize.z);
               radius = _min(obb.m_halfsize.y, obb.m_halfsize.x); obb.m_halfsize.y /= 2.f; obb.m_halfsize.x /= 2.f)
    // radius*=1.4142f;
    Fsphere sphere1, sphere2;
    sphere1.P.add(obb.m_translate, ax);
    sphere1.R = radius * 1.4142f;

    sphere2.P.sub(obb.m_translate, ax);
    sphere2.R = radius / 2.f;

    E->add_Box(obb);
    E->add_Sphere(sphere1);
    E->add_Sphere(sphere2);

    // Physics (Shell)
    m_pPhysicsShell = P_create_Shell();
    R_ASSERT(m_pPhysicsShell);
    m_pPhysicsShell->add_Element(E);
    m_pPhysicsShell->setDensity(2000.f);
    m_pPhysicsShell->SetAirResistance();
}

void CPhysicItem::create_physic_shell()
{
    /// create_box_physic_shell();
    inherited::create_physic_shell();
}

void CPhysicItem::ApplyHudWorldVisual()
{
    if (!m_use_hud_model_as_world || !m_hud_world_visual.c_str() || !m_hud_world_visual.size())
        return;

    const shared_str original_visual = cNameVisual();
    const bool previous_hud_loading = ::Render->hud_loading;
    ::Render->hud_loading = false;
    cNameVisual_set(m_hud_world_visual);
    ::Render->hud_loading = previous_hud_loading;

    if (!smart_cast<IKinematics*>(Visual()))
    {
        Msg("! [%s]: HUD world visual [%s] is not skeletal; restoring [%s]", cNameSect().c_str(), m_hud_world_visual.c_str(),
            original_visual.c_str());
        ::Render->hud_loading = false;
        cNameVisual_set(original_visual);
        ::Render->hud_loading = previous_hud_loading;
        m_use_hud_model_as_world = false;
        m_force_auto_generated_collision = READ_IF_EXISTS(
            pSettings, r_bool, cNameSect().c_str(), "force_auto_generated_collision", false);
        m_world_model_scale = READ_IF_EXISTS(pSettings, r_float, cNameSect().c_str(), "world_scaling", 1.f);
        clamp(m_world_model_scale, 0.001f, 100.f);
        return;
    }

    PlayWorldIdleAnimation();
    spatial_move();
}

void CPhysicItem::PlayWorldIdleAnimation()
{
    IKinematicsAnimated* animated = Visual() ? Visual()->dcast_PKinematicsAnimated() : nullptr;
    if (!animated)
        return;

    const MotionID idle = animated->ID_Cycle_Safe("idle");
    if (!idle.valid())
    {
        Msg("! [%s]: HUD world visual [%s] has no [idle] animation; bind pose will be used", cNameSect().c_str(), cNameVisual().c_str());
        return;
    }

    animated->PlayCycle(idle, FALSE);
    animated->dcast_PKinematics()->CalculateBones_Invalidate();
}

Fmatrix CPhysicItem::renderable_WorldTransform() const
{
    Fmatrix result = XFORM();
    result.i.mul(m_world_model_scale);
    result.j.mul(m_world_model_scale);
    result.k.mul(m_world_model_scale);
    return result;
}

void CPhysicItem::renderable_RenderUI(u32 context_id, IRenderable* root)
{
    if (!Visual())
        return;
    Fmatrix world_transform = renderable_WorldTransform();
    ::Render->add_Visual(context_id, root, Visual(), world_transform);
}

void CPhysicItem::Center(Fvector& center) const
{
    ASSERT_FMT(Visual(), "[%s]: %s[%u] has no visual", __FUNCTION__, cName().c_str(), ID());
    const Fmatrix world_transform = renderable_WorldTransform();
    world_transform.transform_tiny(center, Visual()->getVisData().sphere.P);
}

float CPhysicItem::Radius() const
{
    ASSERT_FMT(Visual(), "[%s]: %s[%u] has no visual", __FUNCTION__, cName().c_str(), ID());
    return Visual()->getVisData().sphere.R * m_world_model_scale;
}

const Fbox& CPhysicItem::BoundingBox() const
{
    ASSERT_FMT(Visual(), "[%s]: %s[%u] has no visual", __FUNCTION__, cName().c_str(), ID());
    m_scaled_bounding_box = Visual()->getVisData().box;
    m_scaled_bounding_box.min.mul(m_world_model_scale);
    m_scaled_bounding_box.max.mul(m_world_model_scale);
    return m_scaled_bounding_box;
}
