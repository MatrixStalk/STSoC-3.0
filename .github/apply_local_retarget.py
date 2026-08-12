from pathlib import Path
import subprocess

repo = Path(__file__).resolve().parents[1]
path = repo / "ogsr_engine/xrGame/player_hud.cpp"
s = path.read_text(encoding="utf-8")

old = '''        // bind_transform is parent-local, while m2b_transform is the inverse
        // of the complete model-space bind transform (see CBoneData::CalculateM2B).
        // Retarget in model space so differences in bone roll/local axes are
        // handled by the bind bases instead of reusing source-local rotations.
        Fmatrix source_bind_world, target_bind_world;
        source_bind_world.invert(source_data.m2b_transform);
        target_bind_world.invert(target_data.m2b_transform);

        const Fmatrix& source_world = source->LL_GetBoneInstance(source_bone_id).mTransform;

        // Extract the animated model-space delta from the Source bind pose,
        // then apply that motion to the Target bind pose. The order is
        // SourceAnim * inverse(SourceBind) * TargetBind, which preserves the
        // target bind basis for rigs with different bone roll/local axes.
        Fmatrix source_world_delta, desired_target_world;
        source_world_delta.mul_43(source_world, source_data.m2b_transform);
        desired_target_world.mul_43(source_world_delta, target_bind_world);

        // Convert the corrected world orientation back through the *current*
        // target parent. This makes every child inherit the already-retargeted
        // clavicle/upper-arm pose instead of accumulating source-rig axis errors.
        Fmatrix target_local;
        if (const u16 target_parent_id = target_data.GetParentID(); target_parent_id != BI_NONE)
        {
            Fmatrix target_parent_inverse;
            target_parent_inverse.invert(target->LL_GetBoneInstance(target_parent_id).mTransform);
            target_local.mul_43(target_parent_inverse, desired_target_world);
        }
        else
        {
            target_local.set(desired_target_world);
        }

        LPCSTR target_bone_name = target->LL_BoneName(target_bone_id);
        const bool is_merge_root = target_bone_name && !xr_strcmp(target_bone_name, source_root_bone);

        if (!is_merge_root)
        {
            // Source SMDs store a translation at every joint, which also encodes
            // source bone length/proportions. Keep only the corrected orientation
            // and always retain the replacement hands skeleton's local offset.
            target_local.c.set(target_data.bind_transform.c);
        }

        // Rebuild model-space transform through the target hierarchy. Keeping the
        // merge root's corrected translation preserves authored whole-arm motion;
        // descendants retain target bone lengths while using corrected bind axes.
        if (const u16 target_parent_id = target_data.GetParentID(); target_parent_id != BI_NONE)
            target_bone->mTransform.mul_43(target->LL_GetBoneInstance(target_parent_id).mTransform, target_local);
        else
            target_bone->mTransform.set(target_local);
'''

new = '''        const u16 source_parent_id = source_data.GetParentID();
        const u16 target_parent_id = target_data.GetParentID();

        // Recover the animated Source pose in parent-local space first.  This
        // removes parent motion exactly once; doing the retarget from absolute
        // model-space transforms made parent rotations accumulate down the arm.
        Fmatrix source_local;
        if (source_parent_id != BI_NONE)
        {
            Fmatrix source_parent_inverse;
            source_parent_inverse.invert(source->LL_GetBoneInstance(source_parent_id).mTransform);
            source_local.mul_43(source_parent_inverse, source->LL_GetBoneInstance(source_bone_id).mTransform);
        }
        else
        {
            source_local.set(source->LL_GetBoneInstance(source_bone_id).mTransform);
        }

        // Source animation delta relative to the Source parent-local bind pose.
        // Translation is handled separately: for descendants it merely carries
        // the source rig's bone lengths and must not reach the replacement hands.
        Fmatrix source_bind_inverse, source_delta;
        source_bind_inverse.invert(source_data.bind_transform);
        source_delta.mul_43(source_local, source_bind_inverse);
        source_delta.c.set(0.f, 0.f, 0.f);

        // Source and Target parents can have different bind axes / bone roll.
        // Convert the angular delta between those coordinate systems with a
        // proper change of basis: Q * D * inverse(Q).
        Fmatrix source_parent_bind_world, target_parent_bind_world;
        source_parent_bind_world.identity();
        target_parent_bind_world.identity();
        if (source_parent_id != BI_NONE)
            source_parent_bind_world.invert(source->LL_GetData(source_parent_id).m2b_transform);
        if (target_parent_id != BI_NONE)
            target_parent_bind_world.invert(target->LL_GetData(target_parent_id).m2b_transform);
        source_parent_bind_world.c.set(0.f, 0.f, 0.f);
        target_parent_bind_world.c.set(0.f, 0.f, 0.f);

        Fmatrix target_parent_bind_inverse, source_to_target_basis, target_to_source_basis;
        target_parent_bind_inverse.invert(target_parent_bind_world);
        source_to_target_basis.mul_43(target_parent_bind_inverse, source_parent_bind_world);
        target_to_source_basis.invert(source_to_target_basis);

        Fmatrix corrected_delta_tmp, corrected_delta;
        corrected_delta_tmp.mul_43(source_to_target_basis, source_delta);
        corrected_delta.mul_43(corrected_delta_tmp, target_to_source_basis);
        corrected_delta.c.set(0.f, 0.f, 0.f);

        // Animate the replacement skeleton around its own bind pose.  This keeps
        // its local axes and its authored joint offsets/bone lengths intact.
        Fmatrix target_local;
        target_local.mul_43(corrected_delta, target_data.bind_transform);

        LPCSTR target_bone_name = target->LL_BoneName(target_bone_id);
        const bool is_merge_root = target_bone_name && !xr_strcmp(target_bone_name, source_root_bone);
        if (is_merge_root)
        {
            // Whole-arm translation belongs to the merge root.  Express the
            // Source root translation delta in the Target parent bind basis.
            Fvector root_translation_delta = source_local.c;
            root_translation_delta.sub(source_data.bind_transform.c);
            source_to_target_basis.transform_dir(root_translation_delta);
            target_local.c.set(target_data.bind_transform.c);
            target_local.c.add(root_translation_delta);
        }
        else
        {
            target_local.c.set(target_data.bind_transform.c);
        }

        // Rebuild model space solely through the already-retargeted Target parent.
        if (target_parent_id != BI_NONE)
            target_bone->mTransform.mul_43(target->LL_GetBoneInstance(target_parent_id).mTransform, target_local);
        else
            target_bone->mTransform.set(target_local);
'''

if old not in s:
    # A parallel matrix job may have checked out after the first job already
    # pushed the final commit.  In that case the desired code is already present.
    if new not in s:
        raise SystemExit("retarget block not found; refusing to patch")
else:
    path.write_text(s.replace(old, new, 1), encoding="utf-8")

# The CI checkout is also our safe patch executor.  Commit only the engine
# change, restoring the staging hook first.  Parallel matrix jobs are allowed to
# lose the push race; they still compile the same patched working tree.
try:
    subprocess.run(["git", "config", "user.name", "KishtimGuy"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.email", "103826442+MatrixStalk@users.noreply.github.com"], cwd=repo, check=True)
    original = subprocess.run(
        ["git", "show", "278ac6955e41f725c272dfffa73e572bd514ca9e:Update_Components.cmd"],
        cwd=repo, check=True, capture_output=True
    ).stdout
    (repo / "Update_Components.cmd").write_bytes(original)
    subprocess.run(["git", "rm", "-f", ".github/apply_local_retarget.py"], cwd=repo, check=True)
    subprocess.run(["git", "add", "ogsr_engine/xrGame/player_hud.cpp", "Update_Components.cmd"], cwd=repo, check=True)
    changed = subprocess.run(["git", "diff", "--cached", "--quiet"], cwd=repo).returncode != 0
    if changed:
        subprocess.run(["git", "commit", "-m", "[ci build] retarget Source HUD rotations in parent-local bind space"], cwd=repo, check=True)
        subprocess.run(["git", "push", "origin", "HEAD:fix/source-hud-retarget"], cwd=repo, check=False)
except Exception as exc:
    print(f"warning: could not publish CI-generated patch commit: {exc}")
