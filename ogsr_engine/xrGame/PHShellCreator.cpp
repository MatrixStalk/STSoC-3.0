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
    if (max_boxes == 1 && boxes.size() > 1)
    {
        // Several overlapping geoms on one small item create competing ground
        // contacts in ODE and make weapons/attachments sink, jump or crawl away.
        // A single envelope is deliberately preferred for dropped inventory items.
        Fvector bounds_min;
        Fvector bounds_max;
        bounds_min.set(flt_max, flt_max, flt_max);
        bounds_max.set(-flt_max, -flt_max, -flt_max);
        for (const SAutoCollisionBox& collision_box : boxes)
        {
            const Fvector& center = collision_box.box.m_translate;
            const Fvector& halfsize = collision_box.box.m_halfsize;
            bounds_min.x = _min(bounds_min.x, center.x - halfsize.x);
            bounds_min.y = _min(bounds_min.y, center.y - halfsize.y);
            bounds_min.z = _min(bounds_min.z, center.z - halfsize.z);
            bounds_max.x = _max(bounds_max.x, center.x + halfsize.x);
            bounds_max.y = _max(bounds_max.y, center.y + halfsize.y);
            bounds_max.z = _max(bounds_max.z, center.z + halfsize.z);
        }

        SAutoCollisionBox envelope;
        envelope.box.m_rotate.identity();
        envelope.box.m_translate.add(bounds_min, bounds_max).mul(0.5f);
        envelope.box.m_halfsize.sub(bounds_max, bounds_min).mul(0.5f);
        boxes.clear();
        boxes.push_back(envelope);
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
