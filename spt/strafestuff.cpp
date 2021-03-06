#include "stdafx.hpp"

#ifdef OE
#include "mathlib.h"
#else
#include "mathlib/mathlib.h"
#endif

#include <iomanip>
#include <sstream>

#include "OrangeBox/cvars.hpp"
#include "OrangeBox/module_hooks.hpp"
#include "OrangeBox/modules.hpp"
#include "OrangeBox/modules/ClientDLL.hpp"
#include "const.h"
#include "strafe_utils.hpp"
#include "strafestuff.hpp"
#include "utils/ent_utils.hpp"
#include "utils/math.hpp"
#include "utils/property_getter.hpp"

#ifndef OE
#include "OrangeBox/overlay/portal_camera.hpp"
#endif

#ifdef max
#undef max
#endif

#ifdef min
#undef min
#endif

// This code is a messed up version of hlstrafe,
// go take a look at that instead:
// https://github.com/HLTAS/hlstrafe

ConVar tas_strafe_version("tas_strafe_version",
                          "2",
                          FCVAR_TAS_RESET,
                          "Strafe version. For backwards compatibility with old scripts.");

ConVar tas_strafe_afh_length("tas_strafe_afh_length", "0.0000000000000000001", FCVAR_TAS_RESET, "Magnitude of AFHs");
ConVar tas_strafe_afh("tas_strafe_afh", "0", FCVAR_TAS_RESET, "Should AFH?");

extern void* gm;

namespace Strafe
{
	bool CanTrace()
	{
		if (!tas_strafe_use_tracing.GetBool())
			return false;

		if (tas_strafe_version.GetInt() == 1)
		{
			return clientDLL.ORIG_UTIL_TraceRay != nullptr;
		}
		else
		{
			return serverDLL.CanTracePlayerBBox();
		}
	}

	static CMoveData* oldmv;
	static void* oldPlayer;
	static CMoveData data;

	void SetMoveData()
	{
		data.m_nPlayerHandle = GetServerPlayer()->GetRefEHandle();
		void** player = reinterpret_cast<void**>((char*)gm + 0x4);
		CMoveData** mv = reinterpret_cast<CMoveData**>((char*)gm + 0x8);
		oldmv = *mv;
		oldPlayer = *player;
		*mv = &data;
		*player = GetServerPlayer();
	}

	void UnsetMoveData()
	{
		void** player = reinterpret_cast<void**>((char*)gm + 0x4);
		CMoveData** mv = reinterpret_cast<CMoveData**>((char*)gm + 0x8);
		*player = oldPlayer;
		*mv = oldmv;
	}

	void TracePlayer(trace_t& trace, const Vector& start, const Vector& end, HullType hull)
	{
#ifndef OE
		if (!CanTrace())
			return;

		float minH;
		float maxH;

		if (tas_strafe_version.GetInt() == 1)
		{
			if (tas_strafe_hull_is_line.GetBool())
			{
				minH = 0;
				maxH = 0;
			}
			else
			{
				minH = -16;
				maxH = 16;
			}

			Vector mins(minH, minH, 0);
			Vector maxs(maxH, maxH, 72);

			if (hull == HullType::DUCKED)
				mins.z = 36;

			Ray_t ray;

			if (hull == HullType::POINT)
				ray.Init(start, end);
			else
				ray.Init(start, end, mins, maxs);

			clientDLL.ORIG_UTIL_TraceRay(ray,
			                             MASK_PLAYERSOLID_BRUSHONLY,
			                             utils::GetClientEntity(0),
			                             COLLISION_GROUP_PLAYER_MOVEMENT,
			                             &trace);
		}
		else
		{
			minH = -16;
			maxH = 16;
			Vector mins(minH, minH, 0);
			Vector maxs(maxH, maxH, 72);

			if (hull == HullType::DUCKED)
				mins.z = 36;

			SetMoveData();
			serverDLL.TracePlayerBBox(start,
			                          end,
			                          mins,
			                          maxs,
			                          MASK_PLAYERSOLID,
			                          COLLISION_GROUP_PLAYER_MOVEMENT,
			                          trace);
			UnsetMoveData();
		}

#endif
	}

	void Trace(trace_t& trace, const Vector& start, const Vector& end)
	{
#ifndef OE
		if (!clientDLL.ORIG_UTIL_TraceRay)
			return;

		Ray_t ray;
		ray.Init(start, end);
		clientDLL.ORIG_UTIL_TraceRay(ray,
		                             MASK_PLAYERSOLID_BRUSHONLY,
		                             utils::GetClientEntity(0),
		                             COLLISION_GROUP_PLAYER_MOVEMENT,
		                             &trace);
#endif
	}

	void FlyMove(PlayerData& player, const MovementVars& vars, PositionType postype);
	int ClipVelocity(Vector& velocty, const Vector& normal, float overbounce);
	static const constexpr double SAFEGUARD_THETA_DIFFERENCE_RAD = M_PI / 65536;

	bool CanUnduck(const PlayerData& player)
	{
		if ((player.DuckPressed && !tas_strafe_autojb.GetBool()) || !tas_strafe_use_tracing.GetBool())
			return false;
		else
		{
			trace_t tr;
			Vector duckedOrigin(player.UnduckedOrigin);
			duckedOrigin.z -= 36;
			TracePlayer(tr, duckedOrigin, player.UnduckedOrigin, Strafe::HullType::DUCKED);

			return tr.fraction == 1.0f;
		}
	}

	PositionType GetPositionType(PlayerData& player, HullType hull)
	{
		// TODO: Check water. If we're under water, return here.
		// Check ground.
		int strafe_version = tas_strafe_version.GetInt();

		if (!tas_strafe_use_tracing.GetBool() || strafe_version == 0 || !CanTrace())
		{
			if (clientDLL.IsGroundEntitySet())
				return PositionType::GROUND;
			else
				return PositionType::AIR;
		}
		else if (strafe_version == 1)
		{
			if (clientDLL.IsGroundEntitySet())
				return PositionType::GROUND;

			if (player.Velocity[2] > 140.f)
				return PositionType::AIR;

			trace_t tr;
			Vector point;
			VecCopy(player.UnduckedOrigin, point);
			point[2] -= 2;

			TracePlayer(tr, player.UnduckedOrigin, point, hull);
			if (tr.plane.normal[2] < 0.7 || tr.m_pEnt == NULL || tr.startsolid)
				return PositionType::AIR;

			if (!tr.startsolid && !tr.allsolid)
				VecCopy<Vector, 3>(tr.endpos, player.UnduckedOrigin);
			return PositionType::GROUND;
		}
		else
		{
			SetMoveData();
			if (player.Velocity[2] > 140.f)
			{
				UnsetMoveData();
				return PositionType::AIR;
			}

			Vector bumpOrigin = player.UnduckedOrigin;
			Vector point = bumpOrigin;
			point[2] -= 2;

			float minH = -16;
			float maxH = 16;
			Vector mins(minH, minH, 0);
			Vector maxs(maxH, maxH, 72);

			if (hull == HullType::DUCKED)
				mins.z = 36;

			trace_t pm;

			serverDLL.TracePlayerBBox(bumpOrigin,
			                          point,
			                          mins,
			                          maxs,
			                          MASK_PLAYERSOLID,
			                          COLLISION_GROUP_PLAYER_MOVEMENT,
			                          pm);

			if (pm.m_pEnt && pm.plane.normal[2] >= 0.7)
			{
				UnsetMoveData();
				return PositionType::GROUND;
			}

			if (DoesGameLookLikePortal())
				serverDLL.ORIG_TracePlayerBBoxForGround2(bumpOrigin,
				                                         point,
				                                         mins,
				                                         maxs,
				                                         GetServerPlayer(),
				                                         MASK_PLAYERSOLID,
				                                         COLLISION_GROUP_PLAYER_MOVEMENT,
				                                         pm);
			else
				serverDLL.ORIG_TracePlayerBBoxForGround(bumpOrigin,
				                                        point,
				                                        mins,
				                                        maxs,
				                                        GetServerPlayer(),
				                                        MASK_PLAYERSOLID,
				                                        COLLISION_GROUP_PLAYER_MOVEMENT,
				                                        pm);

			UnsetMoveData();

			if (pm.m_pEnt && pm.plane.normal[2] >= 0.7)
				return PositionType::GROUND;
			else
				return PositionType::AIR;
		}
	}

	void VectorFME(PlayerData& player,
	               const MovementVars& vars,
	               PositionType postype,
	               double wishspeed,
	               const Vector& a)
	{
		assert(postype != PositionType::WATER);

		bool onground = (postype == PositionType::GROUND);
		double wishspeed_capped = onground ? wishspeed : 30;
		double tmp = wishspeed_capped - DotProduct<Vector, Vector, 2>(player.Velocity, a);
		if (tmp <= 0.0)
			return;

		double accel = onground ? vars.Accelerate : vars.Airaccelerate;
		double accelspeed = accel * wishspeed * vars.EntFriction * vars.Frametime;
		if (accelspeed <= tmp)
			tmp = accelspeed;

		player.Velocity[0] += static_cast<float>(a[0] * tmp);
		player.Velocity[1] += static_cast<float>(a[1] * tmp);
	}

	void CheckVelocity(PlayerData& player, const MovementVars& vars)
	{
		for (std::size_t i = 0; i < 3; ++i)
		{
			if (player.Velocity[i] > vars.Maxvelocity)
				player.Velocity[i] = vars.Maxvelocity;
			if (player.Velocity[i] < -vars.Maxvelocity)
				player.Velocity[i] = -vars.Maxvelocity;
		}
	}

	PositionType Move(PlayerData& player, const MovementVars& vars)
	{
		trace_t tr;

		auto hull = player.Ducking ? HullType::DUCKED : HullType::NORMAL;
		PositionType postype = GetPositionType(player, hull);
		bool onground = (postype == PositionType::GROUND);
		CheckVelocity(player, vars);

		// AddCorrectGravity
		float entGravity = vars.EntGravity;
		if (entGravity == 0.0f)
			entGravity = 1.0f;
		player.Velocity[2] -= static_cast<float>(entGravity * vars.Gravity * 0.5 * vars.Frametime);
		player.Velocity[2] += player.Basevelocity[2] * vars.Frametime;
		player.Basevelocity[2] = 0;
		CheckVelocity(player, vars);

		// Move
		if (onground)
			player.Velocity[2] = 0;

		// Move
		VecAdd(player.Velocity, player.Basevelocity, player.Velocity);
		if (onground)
		{
			// WalkMove
			auto spd = Length(player.Velocity);
			if (spd < 1)
			{
				VecScale(player.Velocity, 0, player.Velocity); // Clear velocity.
			}
			else
			{
				Vector dest;
				VecCopy(player.UnduckedOrigin, dest);
				dest[0] += player.Velocity[0] * vars.Frametime;
				dest[1] += player.Velocity[1] * vars.Frametime;

				TracePlayer(tr, player.UnduckedOrigin, dest, hull);
				if (tr.fraction == 1.0f)
				{
					VecCopy(tr.endpos, player.UnduckedOrigin);
				}
				else
				{
					// Figure out the end position when trying to walk up a step.
					auto playerUp = PlayerData(player);
					dest[2] += vars.Stepsize;
					TracePlayer(tr, playerUp.UnduckedOrigin, dest, hull);
					if (!tr.startsolid && !tr.allsolid)
						VecCopy(tr.endpos, playerUp.UnduckedOrigin);

					FlyMove(playerUp, vars, postype);
					VecCopy(playerUp.UnduckedOrigin, dest);
					dest[2] -= vars.Stepsize;

					TracePlayer(tr, playerUp.UnduckedOrigin, dest, hull);
					if (!tr.startsolid && !tr.allsolid)
						VecCopy(tr.endpos, playerUp.UnduckedOrigin);

					// Figure out the end position when _not_ trying to walk up a step.
					auto playerDown = PlayerData(player);
					FlyMove(playerDown, vars, postype);

					// Take whichever move was the furthest.
					auto downdist =
					    (playerDown.UnduckedOrigin[0] - player.UnduckedOrigin[0])
					        * (playerDown.UnduckedOrigin[0] - player.UnduckedOrigin[0])
					    + (playerDown.UnduckedOrigin[1] - player.UnduckedOrigin[1])
					          * (playerDown.UnduckedOrigin[1] - player.UnduckedOrigin[1]);
					auto updist = (playerUp.UnduckedOrigin[0] - player.UnduckedOrigin[0])
					                  * (playerUp.UnduckedOrigin[0] - player.UnduckedOrigin[0])
					              + (playerUp.UnduckedOrigin[1] - player.UnduckedOrigin[1])
					                    * (playerUp.UnduckedOrigin[1] - player.UnduckedOrigin[1]);

					if ((tr.plane.normal[2] < 0.7) || (downdist > updist))
					{
						VecCopy(playerDown.UnduckedOrigin, player.UnduckedOrigin);
						VecCopy(playerDown.Velocity, player.Velocity);
					}
					else
					{
						VecCopy(playerUp.UnduckedOrigin, player.UnduckedOrigin);
						VecCopy<Vector, 2>(playerUp.Velocity, player.Velocity);
						player.Velocity[2] = playerDown.Velocity[2];
					}
				}
			}
		}
		else
		{
			// AirMove
			FlyMove(player, vars, postype);
		}

		postype = GetPositionType(player, hull);
		VecSubtract(player.Velocity, player.Basevelocity, player.Velocity);
		CheckVelocity(player, vars);
		if (postype != PositionType::GROUND && postype != PositionType::WATER)
		{
			// FixupGravityVelocity
			player.Velocity[2] -= static_cast<float>(entGravity * vars.Gravity * 0.5 * vars.Frametime);
			CheckVelocity(player, vars);
		}

		return postype;
	}

	void FlyMove(PlayerData& player, const MovementVars& vars, PositionType postype)
	{
		const auto MAX_BUMPS = 4;
		const auto MAX_CLIP_PLANES = 5;
		auto hull = player.Ducking ? HullType::DUCKED : HullType::NORMAL;

		trace_t tr;
		Vector originalVelocity, savedVelocity;
		VecCopy(player.Velocity, originalVelocity);
		VecCopy(player.Velocity, savedVelocity);

		auto timeLeft = vars.Frametime;
		auto allFraction = 0.0f;
		auto numPlanes = 0;
		auto blockedState = 0;
		Vector planes[MAX_CLIP_PLANES];

		for (auto bumpCount = 0; bumpCount < MAX_BUMPS; ++bumpCount)
		{
			if (IsZero(player.Velocity))
				break;

			Vector end;
			for (size_t i = 0; i < 3; ++i)
				end[i] = player.UnduckedOrigin[i] + timeLeft * player.Velocity[i];

			TracePlayer(tr, player.UnduckedOrigin, end, hull);

			allFraction += tr.fraction;
			if (tr.allsolid)
			{
				VecScale(player.Velocity, 0, player.Velocity);
				blockedState = 4;
				break;
			}
			if (tr.fraction > 0)
			{
				VecCopy(tr.endpos, player.UnduckedOrigin);
				VecCopy(player.Velocity, savedVelocity);
				numPlanes = 0;
			}
			if (tr.fraction == 1)
				break;

			if (tr.plane.normal[2] > 0.7)
				blockedState |= 1;
			else if (tr.plane.normal[2] == 0)
				blockedState |= 2;

			timeLeft -= timeLeft * tr.fraction;

			if (numPlanes >= MAX_CLIP_PLANES)
			{
				VecScale(player.Velocity, 0, player.Velocity);
				break;
			}

			VecCopy(tr.plane.normal, planes[numPlanes]);
			numPlanes++;

			if (postype != PositionType::GROUND || vars.EntFriction != 1)
			{
				for (auto i = 0; i < numPlanes; ++i)
					if (planes[i][2] > 0.7)
						ClipVelocity(savedVelocity, planes[i], 1);
					else
						ClipVelocity(savedVelocity,
						             planes[i],
						             static_cast<float>(
						                 1.0 + vars.Bounce * (1 - vars.EntFriction)));

				VecCopy(savedVelocity, player.Velocity);
			}
			else
			{
				int i = 0;
				for (i = 0; i < numPlanes; ++i)
				{
					VecCopy(savedVelocity, player.Velocity);
					ClipVelocity(player.Velocity, planes[i], 1);

					int j;
					for (j = 0; j < numPlanes; ++j)
						if (j != i)
							if (DotProduct(player.Velocity, planes[j]) < 0)
								break;

					if (j == numPlanes)
						break;
				}

				if (i == numPlanes)
				{
					if (numPlanes != 2)
					{
						VecScale(player.Velocity, 0, player.Velocity);
						break;
					}

					Vector dir;
					CrossProduct(planes[0], planes[1], dir);
					auto d = static_cast<float>(DotProduct(dir, player.Velocity));
					VecScale(dir, d, player.Velocity);
				}

				if (DotProduct(player.Velocity, originalVelocity) <= 0)
				{
					VecScale(player.Velocity, 0, player.Velocity);
					break;
				}
			}
		}

		if (allFraction == 0)
			VecScale(player.Velocity, 0, player.Velocity);
	}

	int ClipVelocity(Vector& velocty, const Vector& normal, float overbounce)
	{
		const auto STOP_EPSILON = 0.1;

		auto backoff = static_cast<float>(DotProduct(velocty, normal) * overbounce);

		for (size_t i = 0; i < 3; ++i)
		{
			auto change = normal[i] * backoff;
			velocty[i] -= change;

			if (velocty[i] > -STOP_EPSILON && velocty[i] < STOP_EPSILON)
				velocty[i] = 0;
		}

		if (normal[2] > 0)
			return 1;
		else if (normal[2] == 0)
			return 2;
		else
			return 0;
	}

	double TargetTheta(const PlayerData& player,
	                   const MovementVars& vars,
	                   bool onground,
	                   double wishspeed,
	                   double target)
	{
		double accel = onground ? vars.Accelerate : vars.Airaccelerate;
		double L = onground ? vars.Maxspeed : std::min(vars.Maxspeed, (float)30);
		double gamma1 = vars.EntFriction * vars.Frametime * vars.Maxspeed * accel;

		PlayerData copy = player;
		double lambdaVel = copy.Velocity.Length2D();

		double cosTheta;

		if (gamma1 <= 2 * L)
		{
			cosTheta = ((target * target - lambdaVel * lambdaVel) / gamma1 - gamma1) / (2 * lambdaVel);
			return std::acos(cosTheta);
		}
		else
		{
			cosTheta = std::sqrt((target * target - L * L) / lambdaVel * lambdaVel);
			return std::acos(cosTheta);
		}
	}

	double MaxAccelWithCapIntoYawTheta(const PlayerData& player,
	                                   const MovementVars& vars,
	                                   bool onground,
	                                   double wishspeed,
	                                   double vel_yaw,
	                                   double yaw)
	{
		if (!player.Velocity.AsVector2D().IsZero(0))
			vel_yaw = Atan2(player.Velocity.y, player.Velocity.x);

		double theta = MaxAccelTheta(player, vars, onground, wishspeed);

		Vector2D avec(std::cos(theta), std::sin(theta));
		PlayerData vel;
		vel.Velocity.x = player.Velocity.Length2D();
		VectorFME(vel, vars, onground, wishspeed, avec);

		if (vel.Velocity.Length2D() > tas_strafe_capped_limit.GetFloat())
			theta = TargetTheta(player, vars, onground, wishspeed, tas_strafe_capped_limit.GetFloat());

		return std::copysign(theta, NormalizeRad(yaw - vel_yaw));
	}

	double MaxAccelTheta(const PlayerData& player, const MovementVars& vars, bool onground, double wishspeed)
	{
		double accel = onground ? vars.Accelerate : vars.Airaccelerate;
		double accelspeed = accel * wishspeed * vars.EntFriction * vars.Frametime;
		if (accelspeed <= 0.0)
			return M_PI;

		if (player.Velocity.AsVector2D().IsZero(0))
			return 0.0;

		double wishspeed_capped = onground ? wishspeed : vars.WishspeedCap;
		double tmp = wishspeed_capped - accelspeed;
		if (tmp <= 0.0)
			return M_PI / 2;

		double speed = player.Velocity.Length2D();
		if (tmp < speed)
			return std::acos(tmp / speed);

		return 0.0;
	}

	double MaxAccelIntoYawTheta(const PlayerData& player,
	                            const MovementVars& vars,
	                            bool onground,
	                            double wishspeed,
	                            double vel_yaw,
	                            double yaw)
	{
		if (!player.Velocity.AsVector2D().IsZero(0))
			vel_yaw = Atan2(player.Velocity.y, player.Velocity.x);

		double theta = MaxAccelTheta(player, vars, onground, wishspeed);
		if (theta == 0.0 || theta == M_PI)
			return NormalizeRad(yaw - vel_yaw + theta);
		return std::copysign(theta, NormalizeRad(yaw - vel_yaw));
	}

	double MaxAngleTheta(const PlayerData& player,
	                     const MovementVars& vars,
	                     bool onground,
	                     double wishspeed,
	                     bool& safeguard_yaw)
	{
		safeguard_yaw = false;
		double speed = player.Velocity.Length2D();
		double accel = onground ? vars.Accelerate : vars.Airaccelerate;
		double accelspeed = accel * wishspeed * vars.EntFriction * vars.Frametime;

		if (accelspeed <= 0.0)
		{
			double wishspeed_capped = onground ? wishspeed : vars.WishspeedCap;
			accelspeed *= -1;
			if (accelspeed >= speed)
			{
				if (wishspeed_capped >= speed)
					return 0.0;
				else
				{
					safeguard_yaw = true;
					return std::acos(wishspeed_capped
					                 / speed); // The actual angle needs to be _less_ than this.
				}
			}
			else
			{
				if (wishspeed_capped >= speed)
					return std::acos(accelspeed / speed);
				else
				{
					safeguard_yaw = (wishspeed_capped <= accelspeed);
					return std::acos(
					    std::min(accelspeed, wishspeed_capped)
					    / speed); // The actual angle needs to be _less_ than this if wishspeed_capped <= accelspeed.
				}
			}
		}
		else
		{
			if (accelspeed >= speed)
				return M_PI;
			else
				return std::acos(-1 * accelspeed / speed);
		}
	}

	void VectorFME(PlayerData& player, const MovementVars& vars, bool onground, double wishspeed, const Vector2D& a)
	{
		double wishspeed_capped = onground ? wishspeed : vars.WishspeedCap;
		double tmp = wishspeed_capped - player.Velocity.AsVector2D().Dot(a);
		if (tmp <= 0.0)
			return;

		double accel = onground ? vars.Accelerate : vars.Airaccelerate;
		double accelspeed = accel * wishspeed * vars.EntFriction * vars.Frametime;
		if (accelspeed <= tmp)
			tmp = accelspeed;

		player.Velocity.x += static_cast<float>(a.x * tmp);
		player.Velocity.y += static_cast<float>(a.y * tmp);
	}

	double ButtonsPhi(Button button)
	{
		switch (button)
		{
		case Button::FORWARD:
			return 0;
		case Button::FORWARD_LEFT:
			return M_PI / 4;
		case Button::LEFT:
			return M_PI / 2;
		case Button::BACK_LEFT:
			return 3 * M_PI / 4;
		case Button::BACK:
			return -M_PI;
		case Button::BACK_RIGHT:
			return -3 * M_PI / 4;
		case Button::RIGHT:
			return -M_PI / 2;
		case Button::FORWARD_RIGHT:
			return -M_PI / 4;
		default:
			return 0;
		}
	}

	Button GetBestButtons(double theta, bool right)
	{
		if (theta < M_PI / 8)
			return Button::FORWARD;
		else if (theta < 3 * M_PI / 8)
			return right ? Button::FORWARD_RIGHT : Button::FORWARD_LEFT;
		else if (theta < 5 * M_PI / 8)
			return right ? Button::RIGHT : Button::LEFT;
		else if (theta < 7 * M_PI / 8)
			return right ? Button::BACK_RIGHT : Button::BACK_LEFT;
		else
			return Button::BACK;
	}

	void SideStrafeGeneral(const PlayerData& player,
	                       const MovementVars& vars,
	                       bool onground,
	                       double wishspeed,
	                       const StrafeButtons& strafeButtons,
	                       bool useGivenButtons,
	                       Button& usedButton,
	                       double vel_yaw,
	                       double theta,
	                       bool right,
	                       Vector2D& velocity,
	                       double& yaw)
	{
		if (useGivenButtons)
		{
			if (!onground)
			{
				if (right)
					usedButton = strafeButtons.AirRight;
				else
					usedButton = strafeButtons.AirLeft;
			}
			else
			{
				if (right)
					usedButton = strafeButtons.GroundRight;
				else
					usedButton = strafeButtons.GroundLeft;
			}
		}
		else
		{
			usedButton = GetBestButtons(theta, right);
		}
		double phi = ButtonsPhi(usedButton);
		theta = right ? -theta : theta;

		if (!player.Velocity.AsVector2D().IsZero(0))
			vel_yaw = Atan2(player.Velocity.y, player.Velocity.x);

		yaw = NormalizeRad(vel_yaw - phi + theta);

		Vector2D avec(std::cos(yaw + phi), std::sin(yaw + phi));
		PlayerData pl = player;
		VectorFME(pl, vars, onground, wishspeed, avec);
		velocity = pl.Velocity.AsVector2D();
	}

	double YawStrafeMaxAccel(PlayerData& player,
	                         const MovementVars& vars,
	                         bool onground,
	                         double wishspeed,
	                         const StrafeButtons& strafeButtons,
	                         bool useGivenButtons,
	                         Button& usedButton,
	                         double vel_yaw,
	                         double yaw)
	{
		double resulting_yaw;
		double theta = MaxAccelIntoYawTheta(player, vars, onground, wishspeed, vel_yaw, yaw);
		Vector2D newvel;
		SideStrafeGeneral(player,
		                  vars,
		                  onground,
		                  wishspeed,
		                  strafeButtons,
		                  useGivenButtons,
		                  usedButton,
		                  vel_yaw,
		                  std::fabs(theta),
		                  (theta < 0),
		                  newvel,
		                  resulting_yaw);
		player.Velocity.AsVector2D() = newvel;

		return resulting_yaw;
	}

	double YawStrafeCapped(PlayerData& player,
	                       const MovementVars& vars,
	                       bool onground,
	                       double wishspeed,
	                       const StrafeButtons& strafeButtons,
	                       bool useGivenButtons,
	                       Button& usedButton,
	                       double vel_yaw,
	                       double yaw)
	{
		double resulting_yaw;
		double theta = MaxAccelWithCapIntoYawTheta(player, vars, onground, wishspeed, vel_yaw, yaw);
		Vector2D newvel;
		SideStrafeGeneral(player,
		                  vars,
		                  onground,
		                  wishspeed,
		                  strafeButtons,
		                  useGivenButtons,
		                  usedButton,
		                  vel_yaw,
		                  std::fabs(theta),
		                  (theta < 0),
		                  newvel,
		                  resulting_yaw);
		player.Velocity.AsVector2D() = newvel;

		return resulting_yaw;
	}

	double YawStrafeMaxAngle(PlayerData& player,
	                         const MovementVars& vars,
	                         bool onground,
	                         double wishspeed,
	                         const StrafeButtons& strafeButtons,
	                         bool useGivenButtons,
	                         Button& usedButton,
	                         double vel_yaw,
	                         double yaw)
	{
		bool safeguard_yaw;
		double theta = MaxAngleTheta(player, vars, onground, wishspeed, safeguard_yaw);
		if (!player.Velocity.AsVector2D().IsZero(0.0f))
			vel_yaw = Atan2(player.Velocity[1], player.Velocity[0]);

		Vector2D newvel;
		double resulting_yaw;
		SideStrafeGeneral(player,
		                  vars,
		                  onground,
		                  wishspeed,
		                  strafeButtons,
		                  useGivenButtons,
		                  usedButton,
		                  vel_yaw,
		                  theta,
		                  (NormalizeRad(yaw - vel_yaw) < 0),
		                  newvel,
		                  resulting_yaw);

		if (safeguard_yaw)
		{
			Vector2D test_vel1, test_vel2;
			double test_yaw1, test_yaw2;

			SideStrafeGeneral(player,
			                  vars,
			                  onground,
			                  wishspeed,
			                  strafeButtons,
			                  useGivenButtons,
			                  usedButton,
			                  vel_yaw,
			                  std::min(theta - SAFEGUARD_THETA_DIFFERENCE_RAD, 0.0),
			                  (NormalizeRad(yaw - vel_yaw) < 0),
			                  test_vel1,
			                  test_yaw1);
			SideStrafeGeneral(player,
			                  vars,
			                  onground,
			                  wishspeed,
			                  strafeButtons,
			                  useGivenButtons,
			                  usedButton,
			                  vel_yaw,
			                  std::max(theta + SAFEGUARD_THETA_DIFFERENCE_RAD, 0.0),
			                  (NormalizeRad(yaw - vel_yaw) < 0),
			                  test_vel2,
			                  test_yaw2);

			double cos_test1 = test_vel1.Dot(player.Velocity.AsVector2D())
			                   / (player.Velocity.Length2D() * test_vel1.Length());
			double cos_test2 = test_vel2.Dot(player.Velocity.AsVector2D())
			                   / (player.Velocity.Length2D() * test_vel2.Length());
			double cos_newvel =
			    newvel.Dot(player.Velocity.AsVector2D()) / (player.Velocity.Length2D() * newvel.Length());

			//DevMsg("cos_newvel = %.8f; cos_test1 = %.8f; cos_test2 = %.8f\n", cos_newvel, cos_test1, cos_test2);

			if (cos_test1 < cos_newvel)
			{
				if (cos_test2 < cos_test1)
				{
					newvel = test_vel2;
					resulting_yaw = test_yaw2;
					cos_newvel = cos_test2;
				}
				else
				{
					newvel = test_vel1;
					resulting_yaw = test_yaw1;
					cos_newvel = cos_test1;
				}
			}
			else if (cos_test2 < cos_newvel)
			{
				newvel = test_vel2;
				resulting_yaw = test_yaw2;
				cos_newvel = cos_test2;
			}
		}
		else
		{
			//DevMsg("theta = %.08f, yaw = %.08f, vel_yaw = %.08f, speed = %.08f\n", theta, yaw, vel_yaw, player.Velocity.Length2D());
		}

		player.Velocity.AsVector2D() = newvel;
		return resulting_yaw;
	}

	void MapSpeeds(ProcessedFrame& out, const MovementVars& vars)
	{
		if (out.Forward)
		{
			out.ForwardSpeed += vars.Maxspeed;
		}
		if (out.Back)
		{
			out.ForwardSpeed -= vars.Maxspeed;
		}
		if (out.Right)
		{
			out.SideSpeed += vars.Maxspeed;
		}
		if (out.Left)
		{
			out.SideSpeed -= vars.Maxspeed;
		}
	}

	bool StrafeJump(bool jumped,
	                PlayerData& player,
	                const MovementVars& vars,
	                bool ducking,
	                ProcessedFrame& out,
	                bool yawChanged)
	{
		if (!jumped)
			return false;

		out.Jump = true;
		out.Processed = true;

		if (yawChanged && !tas_strafe_allow_jump_override.GetBool())
		{
			// Yaw changed and override not permitted
			return true;
		}
		else if (tas_strafe_jumptype.GetInt() == 2)
		{
			// OE bhop
			out.Yaw = NormalizeDeg(tas_strafe_yaw.GetFloat());
			out.Forward = true;
			MapSpeeds(out, vars);

			return true;
		}
		else if (tas_strafe_jumptype.GetInt() == 1)
		{
			float cap = vars.Maxspeed * ((ducking || (vars.Maxspeed == 320)) ? 0.1 : 0.5);
			float speed = player.Velocity.Length2D();

			if (speed >= cap)
			{
				// Above ABH speed
				if (tas_strafe_afh.GetBool())
				{ // AFH
					out.Yaw = tas_strafe_yaw.GetFloat();
					out.ForwardSpeed = -tas_strafe_afh_length.GetFloat();
					return true;
				}
				else
				{
					// ABH
					out.Yaw = NormalizeDeg(tas_strafe_yaw.GetFloat() + 180);
					return true;
				}
			}
			else
			{
				// Below ABH speed, dont do anything
				return false;
			}
		}
		else if (tas_strafe_jumptype.GetInt() == 3)
		{
			// Glitchless bhop
			const Vector vel = player.Velocity;
			out.Yaw = NormalizeRad(Atan2(player.Velocity[1], player.Velocity[0])) * M_RAD2DEG;
			out.Forward = true;
			MapSpeeds(out, vars);

			return true;
		}
		else
		{
			// Invalid jump type set
			out.Processed = false;
			return false;
		}
	}

	void StrafeVectorial(PlayerData& player,
	                     const MovementVars& vars,
	                     bool jumped,
	                     bool ducking,
	                     StrafeType type,
	                     StrafeDir dir,
	                     double target_yaw,
	                     double vel_yaw,
	                     ProcessedFrame& out,
	                     bool yawChanged)
	{
		if (StrafeJump(jumped, player, vars, ducking, out, yawChanged))
		{
			return;
		}

		ProcessedFrame dummy;
		Strafe(
		    player,
		    vars,
		    jumped,
		    ducking,
		    type,
		    dir,
		    target_yaw,
		    vel_yaw,
		    dummy,
		    StrafeButtons(),
		    true); // Get the desired strafe direction by calling the Strafe function while using forward strafe buttons

		// If forward is pressed, strafing should occur
		if (dummy.Forward)
		{
			if (!yawChanged && tas_strafe_vectorial_increment.GetFloat() > 0)
			{
				// Calculate updated yaw
				double adjustedTarget =
				    NormalizeDeg(target_yaw + tas_strafe_vectorial_offset.GetFloat());
				double normalizedDiff = NormalizeDeg(adjustedTarget - vel_yaw);
				double additionAbs =
				    std::min(static_cast<double>(tas_strafe_vectorial_increment.GetFloat()),
				             std::abs(normalizedDiff));

				// Snap to target if difference too large (likely due to an ABH)
				if (std::abs(normalizedDiff) > tas_strafe_vectorial_snap.GetFloat())
					out.Yaw = adjustedTarget;
				else
					out.Yaw = vel_yaw + std::copysign(additionAbs, normalizedDiff);
			}
			else
				out.Yaw = vel_yaw;

			// Set move speeds to match the current yaw to produce the acceleration in direction thetaDeg
			double thetaDeg = dummy.Yaw;
			double diff = (out.Yaw - thetaDeg) * M_DEG2RAD;
			out.ForwardSpeed = static_cast<float>(std::cos(diff) * vars.Maxspeed);
			out.SideSpeed = static_cast<float>(std::sin(diff) * vars.Maxspeed);
			out.Processed = true;
		}
	}

	bool Strafe(PlayerData& player,
	            const MovementVars& vars,
	            bool jumped,
	            bool ducking,
	            StrafeType type,
	            StrafeDir dir,
	            double target_yaw,
	            double vel_yaw,
	            ProcessedFrame& out,
	            const StrafeButtons& strafeButtons,
	            bool useGivenButtons)
	{
		//DevMsg("[Strafing] ducking = %d\n", (int)ducking);
		if (StrafeJump(jumped,
		               player,
		               vars,
		               ducking,
		               out,
		               false)) // yawChanged == false when calling this function
		{
			return vars.OnGround;
		}

		double wishspeed = vars.Maxspeed;
		if (vars.ReduceWishspeed)
			wishspeed *= 0.33333333f;

		Button usedButton = Button::FORWARD;
		bool strafed;
		strafed = true;

		switch (dir)
		{
		case StrafeDir::YAW:
			if (type == StrafeType::MAXACCEL)
				out.Yaw = YawStrafeMaxAccel(player,
				                            vars,
				                            vars.OnGround,
				                            wishspeed,
				                            strafeButtons,
				                            useGivenButtons,
				                            usedButton,
				                            vel_yaw * M_DEG2RAD,
				                            target_yaw * M_DEG2RAD)
				          * M_RAD2DEG;
			else if (type == StrafeType::MAXANGLE)
				out.Yaw = YawStrafeMaxAngle(player,
				                            vars,
				                            vars.OnGround,
				                            wishspeed,
				                            strafeButtons,
				                            useGivenButtons,
				                            usedButton,
				                            vel_yaw * M_DEG2RAD,
				                            target_yaw * M_DEG2RAD)
				          * M_RAD2DEG;
			else if (type == StrafeType::CAPPED)
				out.Yaw = YawStrafeCapped(player,
				                          vars,
				                          vars.OnGround,
				                          wishspeed,
				                          strafeButtons,
				                          useGivenButtons,
				                          usedButton,
				                          vel_yaw * M_DEG2RAD,
				                          target_yaw * M_DEG2RAD)
				          * M_RAD2DEG;
			else if (type == StrafeType::DIRECTION)
				out.Yaw = target_yaw;
			break;
		default:
			strafed = false;
			break;
		}

		if (strafed)
		{
			out.Forward = (usedButton == Button::FORWARD || usedButton == Button::FORWARD_LEFT
			               || usedButton == Button::FORWARD_RIGHT);
			out.Back = (usedButton == Button::BACK || usedButton == Button::BACK_LEFT
			            || usedButton == Button::BACK_RIGHT);
			out.Right = (usedButton == Button::RIGHT || usedButton == Button::FORWARD_RIGHT
			             || usedButton == Button::BACK_RIGHT);
			out.Left = (usedButton == Button::LEFT || usedButton == Button::FORWARD_LEFT
			            || usedButton == Button::BACK_LEFT);
			out.Processed = true;
			MapSpeeds(out, vars);
		}

		return vars.OnGround;
	}

	void Friction(PlayerData& player, bool onground, const MovementVars& vars)
	{
		if (!onground)
			return;

		// Doing all this in floats, mismatch is too real otherwise.
		auto speed = player.Velocity.Length();
		if (speed < 0.1)
			return;

		auto friction = float{vars.Friction * vars.EntFriction};
		auto control = (speed < vars.Stopspeed) ? vars.Stopspeed : speed;
		auto drop = control * friction * vars.Frametime;
		auto newspeed = std::max(speed - drop, 0.f);
		player.Velocity *= (newspeed / speed);
	}

	ConVar tas_strafe_lgagst_min("tas_strafe_lgagst_min", "150", FCVAR_TAS_RESET, "");
	ConVar tas_strafe_lgagst_max("tas_strafe_lgagst_max", "270", FCVAR_TAS_RESET, "");

	bool LgagstJump(PlayerData& player, const MovementVars& vars)
	{
		double vel = player.Velocity.Length2D();
		if (vars.OnGround && vel <= tas_strafe_lgagst_max.GetFloat() && vel >= tas_strafe_lgagst_min.GetFloat())
		{
			return true;
		}
		else
		{
			return false;
		}
	}

} // namespace Strafe
