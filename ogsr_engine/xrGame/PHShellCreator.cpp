#include "stdafx.h"
#include "PHShellCreator.h"
#include "PhysicsShell.h"
#include "gameobject.h"
#include "physicsshellholder.h"
#include "inventory_item.h"
#include "../Include/xrRender/Kinematics.h"
#include "../xr_3da/bone.h"

namespace
{
struct SAutoCollisionBox
{
    Fobb box;
    float volume{};
};

bool shell_has_geometry(CPhysicsShell* shell)
{
    if (!shell)
        return false;
    for (u16 i = 0; i < shell->get_ElementsNumber(); ++i)
        if (shell->get_ElementByStoreOrder(i)->has_geoms())
            return true;
    return false;
}

bool create_mesh_derived_shell(CPhysicsShellHolder* owner)
{
    IKinematics* kinematics = owner && owner->Visual() ? smart_cast<IKinematics*>(owner->Visual()) : nullptr;
    if (!kinematics)
        return false;

    xr_vector<SAutoCollisionBox> boxes;
    boxes.reserve(kinematics->RChildCount());
    const float scale = _max(owner->PHCollisionVisualScale(), 0.001f);

    for (u32 mesh = 0; mesh < kinematics->RChildCount(); ++mesh)
    {
        if (!kinematics->GetRFlag(mesh))
            continue;

        Fvector size = kinematics->RC_VisBox(mesh);
        Fvector center = kinematics->RC_VisCenter(mesh);
        size.mul(scale);
        center.mul(scale);
        if (!_valid(size) || !_valid(center) || size.x <= EPS_S || size.y <= EPS_S || size.z <= EPS_S)
            continue;

        SAutoCollisionBox candidate;
        candidate.box.m_rotate.identity();
        candidate.box.m_translate = center;
        candidate.box.m_halfsize.set(size).mul(0.5f);
        candidate.volume = size.x * size.y * size.z;

        const bool duplicate = std::any_of(boxes.begin(), boxes.end(), [&candidate](const SAutoCollisionBox& existing) {
            return existing.box.m_translate.similar(candidate.box.m_translate, 0.002f) &&
                existing.box.m_halfsize.similar(candidate.box.m_halfsize, 0.002f);
        });
        if (!duplicate)
            boxes.push_back(candidate);
    }

    if (boxes.empty())
    {
        Fobb fallback;
        owner->Visual()->getVisData().box.get_CD(fallback.m_translate, fallback.m_halfsize);
        fallback.m_translate.mul(scale);
        fallback.m_halfsize.mul(scale);
        fallback.m_rotate.identity();
        if (!_valid(fallback.m_translate) || !_valid(fallback.m_halfsize) || fallback.m_halfsize.x <= EPS_S ||
            fallback.m_halfsize.y <= EPS_S || fallback.m_halfsize.z <= EPS_S)
            return false;
        boxes.push_back({fallback, fallback.m_halfsize.x * fallback.m_halfsize.y * fallback.m_halfsize.z * 8.f});
    }

    const u32 max_boxes = clampr(owner->PHAutoCollisionMaxBoxes(), 1u, 64u);
    if (max_boxes == 1)
    {
        // Per-mesh boxes of a skeletal HUD model are not guaranteed to use the
        // same up-to-date pose on the frame where its world shell is created.
        // Their union can therefore contain a remote stale mesh and produce an
        // enormous contact correction on the first ground hit. The visual's
        // root bounds are stable and already enclose the complete world model.
        Fobb envelope;
        owner->Visual()->getVisData().box.get_CD(envelope.m_translate, envelope.m_halfsize);
        envelope.m_translate.mul(scale);
        envelope.m_halfsize.mul(scale);
        envelope.m_rotate.identity();
        if (_valid(envelope.m_translate) && _valid(envelope.m_halfsize) && envelope.m_halfsize.x > EPS_S &&
            envelope.m_halfsize.y > EPS_S && envelope.m_halfsize.z > EPS_S)
        {
            boxes.clear();
            boxes.push_back({envelope, envelope.m_halfsize.x * envelope.m_halfsize.y * envelope.m_halfsize.z * 8.f});
        }
    }
    else
    {
        std::sort(boxes.begin(), boxes.end(), [](const SAutoCollisionBox& left, const SAutoCollisionBox& right) {
            return left.volume > right.volume;
        });
        if (boxes.size() > max_boxes)
            boxes.resize(max_boxes);
    }

    // ODE can tunnel through or continuously re-penetrate with millimetre-thin
    // imported sights and mounts. Keep the collision thickness physically useful
    // without changing the rendered model.
    constexpr float min_collision_half_extent = 0.01f;
    for (SAutoCollisionBox& collision_box : boxes)
    {
        collision_box.box.m_halfsize.x = _max(collision_box.box.m_halfsize.x, min_collision_half_extent);
        collision_box.box.m_halfsize.y = _max(collision_box.box.m_halfsize.y, min_collision_half_extent);
        collision_box.box.m_halfsize.z = _max(collision_box.box.m_halfsize.z, min_collision_half_extent);
    }

    CPhysicsElement* element = P_create_Element();
    if (!element)
        return false;
    for (const SAutoCollisionBox& collision_box : boxes)
        element->add_Box(collision_box.box);

    CPhysicsShell* shell = P_create_Shell();
    if (!shell)
    {
        xr_delete(element);
        return false;
    }

    shell->add_Element(element);
    const CBoneData& root_bone = kinematics->LL_GetData(kinematics->LL_GetBoneRoot());
    float shell_mass = _max(root_bone.mass, 0.01f);
    if (const CInventoryItem* inventory_item = smart_cast<CInventoryItem*>(owner))
        shell_mass = _max(inventory_item->Weight(), 0.05f);
    shell->setMass(shell_mass);
    shell->SetMaterial(root_bone.game_mtl_idx);

    // A freshly activated, long and thin inventory box may begin its first
    // ground step with a relatively deep contact. ODE's generic 150 m/s limit
    // lets the one-frame correction launch the item across the level before
    // the contact manifold becomes stable. Exact integration reduces that
    // initial overshoot, and the item-specific limit remains above ordinary
    // drops and the burer's configured 8 m/s weapon throw.
    constexpr float auto_collision_linear_limit = 12.f;
    constexpr float auto_collision_angular_limit = 10.f;
    shell->SetPrefereExactIntegration();
    shell->set_DynamicLimits(auto_collision_linear_limit, auto_collision_angular_limit);
    owner->PPhysicsShell() = shell;

    Msg("* Auto collision [%s]: generated %u mesh box(es), scale [%g]", owner->cNameSect().c_str(), boxes.size(), scale);
    return true;
}
} // namespace

void CPHShellSimpleCreator::CreatePhysicsShell()
{
    CPhysicsShellHolder* owner = smart_cast<CPhysicsShellHolder*>(this);
    VERIFY(owner);
    if (!owner->Visual())
        return;

    IKinematics* pKinematics = smart_cast<IKinematics*>(owner->Visual());
    VERIFY(pKinematics);

    if (owner->PPhysicsShell())
        return;

    if (owner->PHForceAutoGeneratedCollision() && !create_mesh_derived_shell(owner))
        Msg("! Auto collision [%s]: mesh-derived shell creation failed, trying authored shapes", owner->cNameSect().c_str());

    if (!owner->PPhysicsShell())
    {
        owner->PPhysicsShell() = P_create_Shell();
#ifdef DEBUG
        owner->PPhysicsShell()->dbg_obj = owner;
#endif
        owner->m_pPhysicsShell->build_FromKinematics(pKinematics, 0);

        if (!shell_has_geometry(owner->PPhysicsShell()) && owner->PHUseAutoGeneratedCollision())
        {
            xr_delete(owner->PPhysicsShell());
            if (!create_mesh_derived_shell(owner))
                Msg("! Auto collision [%s]: fallback generation failed", owner->cNameSect().c_str());
        }
    }

    if (!owner->PPhysicsShell())
        return;

#ifdef DEBUG
    owner->PPhysicsShell()->dbg_obj = owner;
#endif

    if (owner->m_pPhysicsShell->get_ElementsNumber() == 0)
    {
        Msg(" ! Error: world item visual [%s] has no elements!", pKinematics->getDebugName().c_str());
    }
    else if (!owner->m_pPhysicsShell->get_ElementByStoreOrder(0)->has_geoms())
    {
        Msg(" ! Error: world item visual [%s] has no shape!", pKinematics->getDebugName().c_str());
    }

    owner->PPhysicsShell()->set_PhysicsRefObject(owner);
    // m_pPhysicsShell->SmoothElementsInertia(0.3f);
    owner->PPhysicsShell()->mXFORM.set(owner->XFORM());
    owner->PPhysicsShell()->SetAirResistance(0.001f, 0.02f);
}
