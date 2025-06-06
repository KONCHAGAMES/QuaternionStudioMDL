﻿//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// studiomdl.c: generates a studio .mdl file from a .qc script
// models/<scriptname>.mdl.
//
// $NoKeywords: $
//
//===========================================================================//


#pragma warning( disable : 4244 )
#pragma warning( disable : 4237 )
#pragma warning( disable : 4305 )


#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <math.h>
#include <float.h>

#include "cmdlib.h"
#include "scriplib.h"
#include "mathlib/mathlib.h"
#include "studio.h"
#include "studiomdl.h"
#include "bone_setup.h"
#include "tier1/strtools.h"
#include "mathlib/vmatrix.h"
#include "mdlobjects/dmeboneflexdriver.h"
#include "tier1/utlspheretree.h"


class CBoneRenderBounds
{
public:
	Vector m_Mins;	// In bone space.
	Vector m_Maxs;
};

// this is computed once so render models and their physics hulls get translated by the same amount
static Vector g_PropCenterOffset(0,0,0);


//----------------------------------------------------------------------
// underlay:
// studiomdl : delta = new_anim * ( -1 * base_anim )
// engine : result = (w * delta) * base_anim
// 
// overlay
// 
// studiomdl : delta = (-1 * base_anim ) * new_anim
// engine : result = base_anim * (w * delta)
//
//----------------------------------------------------------------------
void QuaternionSMAngles( float s, Quaternion const &p, Quaternion const &q, RadianEuler &angles )
{
	Quaternion qt;
	QuaternionSM( s, p, q, qt );
	QuaternionAngles( qt, angles );
}


void QuaternionMAAngles( Quaternion const &p, float s, Quaternion const &q, RadianEuler &angles )
{
	Quaternion qt;
	QuaternionMA( p, s, q, qt );
	QuaternionAngles( qt, angles );
}


// q = p * (-q * p)

//-----------------------------------------------------------------------------
// Purpose: subtract linear motion from root bone animations
//			fixup missing frames from looping animations
//			create "delta" animations
//-----------------------------------------------------------------------------
int g_rootIndex = 0;


void buildAnimationWeights( void );
void extractLinearMotion( s_animation_t *panim, int motiontype, int iStartFrame, int iEndFrame, int iSrcFrame, s_animation_t *pRefAnim, int iRefFrame /* , Vector pos, QAngle angles */ );
void fixupMissingFrame( s_animation_t *panim );
void realignLooping( s_animation_t *panim );
void extractUnusedMotion( s_animation_t *panim );

// TODO: psrc and pdest as terms are ambigious, replace with something better
void setAnimationWeight( s_animation_t *panim, int index );
void processMatch( s_animation_t *psrc, s_animation_t *pdest, int flags );
void worldspaceBlend( s_animation_t *psrc, s_animation_t *pdest, int srcframe, int flags );
void processAutoorigin( s_animation_t *psrc, s_animation_t *pdest, int flags, int srcframe, int destframe, int bone );
void subtractBaseAnimations( s_animation_t *psrc, s_animation_t *pdest, int srcframe, int flags );
void fixupLoopingDiscontinuities( s_animation_t *panim, int start, int end );
void matchBlend( s_animation_t *pDestAnim, s_animation_t *pSrcAnimation, int iSrcFrame, int iDestFrame, int iPre, int iPost );
void makeAngle( s_animation_t *panim, float angle );
void fixupIKErrors( s_animation_t *panim, s_ikrule_t *pRule );
void createDerivative( s_animation_t *panim, float scale );
void clearAnimations( s_animation_t *panim, bool bRetainDuration = false );
void counterRotateBone( s_animation_t *panim, int bone, QAngle target );
void localHierarchy( s_animation_t *panim, char *pBonename, char *pParentname, int start, int peak, int tail, int end  );

void linearDelta( s_animation_t *psrc, s_animation_t *pdest, int srcframe, int flags );
void splineDelta( s_animation_t *psrc, s_animation_t *pdest, int srcframe, int flags );
void reencodeAnimation( s_animation_t *panim, int frameskip );
void forceNumframes( s_animation_t *panim, int frames );

void forceAnimationLoop( s_animation_t *panim );

void solveBone( s_animation_t *panim, int iFrame, int iBone, matrix3x4_t* pBoneToWorld );

void DumpDefineBones( void );


void ClearModel (void)
{

}


float DriverHelperRanges( int nTarget, int nStart, int nPeak, int nTail, int nEnd )
{
	// returns a SMOOTH 0..1 scale of a target within start, peak, tail, end range values

	if ( nTarget <= nStart || nTarget >= nEnd )
		return 0;

	if ( nTarget >= nPeak && nTarget <= nTail )
		return 1;

	if ( nTarget > nStart && nTarget < nPeak )
		return clamp( smoothstep_bounds( (float)nStart, (float)nPeak, (float)nTarget ), 0, 1 );

	return clamp( smoothstep_bounds( (float)nEnd, (float)nTail, (float)nTarget ), 0, 1 );
}


void processAnimations()
{ 
	int i, j;

	// find global root bone.
	if ( strlen( rootname ) )
	{
		g_rootIndex = findGlobalBone( rootname );
		if (g_rootIndex == -1)
			g_rootIndex = 0;
	}

	buildAnimationWeights( );

	for (i = 0; i < g_numani; i++)
	{
		s_animation_t *panim = g_panimation[i];

		extractUnusedMotion( panim ); // FIXME: this should be part of LinearMotion()

		setAnimationWeight( panim, 0 );

		int startframe = 0;

		if (panim->fudgeloop)
		{
			fixupMissingFrame( panim );
		}

		for (j = 0; j < panim->numcmds; j++)
		{
			s_animcmd_t *pcmd = &panim->cmds[j];

			switch( pcmd->cmd )
			{
			case CMD_WEIGHTS:
				setAnimationWeight( panim, pcmd->u.weightlist.index );
				break;
			case CMD_SUBTRACT:
				panim->flags |= STUDIO_DELTA;
				subtractBaseAnimations( pcmd->u.subtract.ref, panim, pcmd->u.subtract.frame, pcmd->u.subtract.flags );
				break;
			case CMD_AO:
				{
					int bone = g_rootIndex;
					if (pcmd->u.ao.pBonename != NULL)
					{
						bone = findGlobalBone( pcmd->u.ao.pBonename );
						if (bone == -1)
						{
							MdlError("unable to find bone %s to alignbone\n", pcmd->u.ao.pBonename );
						}
					}
					processAutoorigin( pcmd->u.ao.ref, panim, pcmd->u.ao.motiontype, pcmd->u.ao.srcframe, pcmd->u.ao.destframe, bone );
				}
				break;
			case CMD_MATCH:
				processMatch( pcmd->u.match.ref, panim, false );
				break;
			case CMD_FIXUP:
				fixupLoopingDiscontinuities( panim, pcmd->u.fixuploop.start, pcmd->u.fixuploop.end );
				break;
			case CMD_ANGLE:
				makeAngle( panim, pcmd->u.angle.angle );
				break;
			case CMD_IKFIXUP:
				break;
			case CMD_IKRULE:
				// processed later
				break;
			case CMD_MOTION:
				{
					extractLinearMotion( 
						panim, 
						pcmd->u.motion.motiontype, 
						startframe, 
						pcmd->u.motion.iEndFrame, 
						pcmd->u.motion.iEndFrame, 
						panim, 
						startframe );
					startframe = pcmd->u.motion.iEndFrame;
				}
				break;
			case CMD_REFMOTION:
				{
					extractLinearMotion(
						panim, 
						pcmd->u.motion.motiontype, 
						startframe, 
						pcmd->u.motion.iEndFrame, 
						pcmd->u.motion.iSrcFrame, 
						pcmd->u.motion.pRefAnim, 
						pcmd->u.motion.iRefFrame );
					startframe = pcmd->u.motion.iEndFrame;
				}
				break;
			case CMD_DERIVATIVE:
				{
					createDerivative(
						panim, 
						pcmd->u.derivative.scale );
				}
				break;
			case CMD_NOANIMATION:
				{
					clearAnimations( panim );
				}
				break;
			case CMD_NOANIM_KEEPDURATION:
				{
					clearAnimations( panim, true );
				}
				break;
			case CMD_LINEARDELTA:
				{
					panim->flags |= STUDIO_DELTA;
					linearDelta( panim, panim, panim->numframes - 1, pcmd->u.linear.flags );
				}
				break;
			case CMD_COMPRESS:
				{
					reencodeAnimation( panim, pcmd->u.compress.frames );
				}
				break;
			case CMD_NUMFRAMES:
				{
					forceNumframes( panim, pcmd->u.numframes.frames );
				}
				break;
			case CMD_COUNTERROTATE:
				{
					int bone = findGlobalBone( pcmd->u.counterrotate.pBonename );
					if (bone != -1)
					{
						QAngle target;

						if (!pcmd->u.counterrotate.bHasTarget)
						{
							matrix3x4_t rootxform;
							matrix3x4_t defaultBoneToWorld;
							AngleMatrix( panim->rotation, rootxform );
							ConcatTransforms( rootxform, g_bonetable[bone].boneToPose, defaultBoneToWorld );

							MatrixAngles( defaultBoneToWorld, target );
						}
						else
						{
							target.Init( pcmd->u.counterrotate.targetAngle[0], pcmd->u.counterrotate.targetAngle[1], pcmd->u.counterrotate.targetAngle[2] );
						}

						counterRotateBone( panim, bone, target );
					}
					else
					{
						MdlError("unable to find bone %s to counterrotate\n", pcmd->u.counterrotate.pBonename );
					}
				}
				break;
			case CMD_WORLDSPACEBLEND:
				worldspaceBlend( pcmd->u.world.ref, panim, pcmd->u.world.startframe, pcmd->u.world.loops );
				break;
			case CMD_MATCHBLEND:
				matchBlend( panim, pcmd->u.match.ref, pcmd->u.match.srcframe, pcmd->u.match.destframe, pcmd->u.match.destpre, pcmd->u.match.destpost );
				break;
			case CMD_LOCALHIERARCHY:
				localHierarchy( panim, pcmd->u.localhierarchy.pBonename, pcmd->u.localhierarchy.pParentname, pcmd->u.localhierarchy.start, pcmd->u.localhierarchy.peak, pcmd->u.localhierarchy.tail, pcmd->u.localhierarchy.end );
				// localHierarchy( panim, char	*pBonename, char *pParentname, int start, int peak, int tail, int end );
				break;
			case CMD_FORCEBONEPOSROT:
				{
					int bone = findGlobalBone( pcmd->u.forceboneposrot.pBonename );
					if (bone != -1)
					{
						Vector vecPos = Vector( pcmd->u.forceboneposrot.pos[0], pcmd->u.forceboneposrot.pos[1], pcmd->u.forceboneposrot.pos[2] );
						Quaternion quat = Quaternion(pcmd->u.forceboneposrot.qrot[0], pcmd->u.forceboneposrot.qrot[1], pcmd->u.forceboneposrot.qrot[2], pcmd->u.forceboneposrot.qrot[3]);
						//QAngle angRot = QAngle( pcmd->u.forceboneposrot.rot[0], pcmd->u.forceboneposrot.rot[1], pcmd->u.forceboneposrot.rot[2] );

						matrix3x4_t matRot;
						//AngleMatrix( angRot, matRot );

						QuaternionMatrix(quat, matRot);

						for ( int i=0; i<panim->numframes; i++ )
						{
							if ( pcmd->u.forceboneposrot.bDoPos )
								panim->sanim[i][bone].pos = vecPos;

							if ( pcmd->u.forceboneposrot.bDoRot )
							{
								int nParent = g_bonetable[bone].parent;
								if ( nParent == -1 || pcmd->u.forceboneposrot.bRotIsLocal )
								{
									//panim->sanim[i][bone].rot = RadianEuler( angRot );
									QuaternionCopy(quat, panim->sanim[i][bone].qrot);
								}
								else
								{
									matrix3x4_t srcBoneToWorld[MAXSTUDIOBONES];
									CalcBoneTransforms( panim, i, srcBoneToWorld );

									matrix3x4_t worldToBone;
									MatrixInvert( srcBoneToWorld[nParent], worldToBone );

									matrix3x4_t local;
									ConcatTransforms( worldToBone, matRot, local );

									//RadianEuler angTemp;
									Quaternion qTemp;
									MatrixQuaternion( local, qTemp);

									panim->sanim[i][bone].qrot = qTemp;
								}
							}
						}
					}
					else
					{
						MdlError("unable to find bone %s to foceboneposrot\n", pcmd->u.forceboneposrot.pBonename );
					}
				}
				break;
			case CMD_BONEDRIVER:
				{
					int bone = findGlobalBone( pcmd->u.bonedriver.pBonename );
					if (bone != -1)
					{
						for ( int i=0; i<panim->numframes; i++ )
						{
							float flCurrentValue = panim->sanim[i][bone].pos[pcmd->u.bonedriver.iAxis];

							if ( pcmd->u.bonedriver.all )
							{
								panim->sanim[i][bone].pos[pcmd->u.bonedriver.iAxis] = pcmd->u.bonedriver.value;
							}
							else
							{
								float flDriverWeightAtThisFrame = DriverHelperRanges( i, pcmd->u.bonedriver.start, pcmd->u.bonedriver.peak, pcmd->u.bonedriver.tail, pcmd->u.bonedriver.end );
								panim->sanim[i][bone].pos[pcmd->u.bonedriver.iAxis] = Lerp( flDriverWeightAtThisFrame, flCurrentValue, pcmd->u.bonedriver.value );
							}
						}
					}
					else
					{
						MdlError("unable to find bone %s\n", pcmd->u.bonedriver.pBonename );
					}
				}
				break;
			case CMD_REVERSE:
				{
					int iCountFrames = panim->numframes-1;
					for ( int i=0; i<iCountFrames/2; i++ )
					{
						for ( int n=g_numbones-1; n>=0; n-- )
						{
							Vector posTemp;
							//RadianEuler rotTemp;
							Quaternion rotTemp;

							VectorCopy( panim->sanim[i][n].pos, posTemp );
							QuaternionCopy( panim->sanim[i][n].qrot, rotTemp );

							VectorCopy( panim->sanim[iCountFrames-i][n].pos, panim->sanim[i][n].pos );
							QuaternionCopy( panim->sanim[iCountFrames-i][n].qrot, panim->sanim[i][n].qrot );

							VectorCopy( posTemp, panim->sanim[iCountFrames-i][n].pos );
							QuaternionCopy( rotTemp, panim->sanim[iCountFrames-i][n].qrot );
						}
					}
				}
				break;
			case CMD_APPENDANIM:
				{
					s_animation_t *pAppendAnimation = pcmd->u.appendanim.ref;
					
					int iPrevNumFrames = panim->numframes;
					forceNumframes( panim, panim->numframes + pAppendAnimation->numframes );

					for ( int i=iPrevNumFrames; i<panim->numframes; i++ )
					{
						for ( int n=g_numbones-1; n>=0; n-- )
						{
							VectorCopy( pAppendAnimation->sanim[i-iPrevNumFrames][n].pos, panim->sanim[i][n].pos );
							QuaternionCopy( pAppendAnimation->sanim[i-iPrevNumFrames][n].qrot, panim->sanim[i][n].qrot );
						}
					}
				}
				break;
			}
		}

		if (panim->motiontype)
		{
			int lastframe;
			if (!(panim->flags & STUDIO_LOOPING) )
			{
				// roll back 0.2 seconds to try to prevent popping
				int frames = panim->fps * panim->motionrollback;
				lastframe = MAX( MIN( startframe + 1, panim->numframes - 1), panim->numframes - frames - 1 );
				//printf("%s : %d %d (%d)\n", panim->name, startframe, lastframe, panim->numframes - 1 );
			}
			else
			{
				lastframe = panim->numframes - 1;
			}
			extractLinearMotion( panim, panim->motiontype, startframe, lastframe, panim->numframes - 1, panim, startframe );
			startframe = panim->numframes - 1;
		}

		realignLooping( panim );
		
		if ( !( panim->flags & STUDIO_NOFORCELOOP ) )
		{
			forceAnimationLoop( panim );
		}
	}

	// merge weightlists
	for (i = 0; i < g_sequence.Count(); i++)
	{
		int k, n;
		for (n = 0; n < g_numbones; n++)
		{
			g_sequence[i].weight[n] = 0.0;

			for (j = 0; j < g_sequence[i].groupsize[0]; j++)
			{
				for (k = 0; k < g_sequence[i].groupsize[1]; k++)
				{
					g_sequence[i].weight[n] = MAX( g_sequence[i].weight[n], g_sequence[i].panim[j][k]->weight[n] );
				}
			}
		}
		// force parent bones to non-zero weight if worldspace blend
		if (g_sequence[i].flags & STUDIO_WORLD)
		{
			for (n = g_numbones - 1; n >= 0; n--)
			{
				if (g_sequence[i].weight[n] && g_bonetable[n].parent != -1 && g_sequence[i].weight[g_bonetable[n].parent] == 0.0)
				{
					g_sequence[i].weight[g_bonetable[n].parent] = 1.0;
					// printf("%s : %d %d\n", g_sequence[i].name, n, g_bonetable[n].parent );
				}
				for (j = 0; j < g_sequence[i].groupsize[0]; j++)
				{
					for (k = 0; k < g_sequence[i].groupsize[1]; k++)
					{
						if (g_sequence[i].panim[j][k]->weight[n] && g_bonetable[n].parent != -1 && g_sequence[i].panim[j][k]->weight[g_bonetable[n].parent] == 0.0)
						{
							g_sequence[i].panim[j][k]->weight[g_bonetable[n].parent] = 0.001;
							// printf("%s : %d %d\n", g_sequence[i].panim[j][k]->name, n, g_bonetable[n].parent );
						}
					}
				}
			}
		}
	}
}


/*
void lookupLinearMotion( s_animation_t *panim, int motiontype, int startframe, int endframe, Vector &p1, Vector &p2 )
{
	Vector p0 = panim->sanim[startframe][g_rootIndex].pos;
	p2 = panim->sanim[endframe][g_rootIndex].pos[0];

	float fFrame = (startframe + endframe) / 2.0;
	int iFrame = (int)fFrame;
	float s = fFrame - iFrame;

	p1 = panim->sanim[iFrame][g_rootIndex].pos * (1 - s) + panim->sanim[iFrame+1][g_rootIndex].pos * s;
}
*/


	// 0.375 * v1 + 0.125 * v2 - d2 = 0.5 * v1 + 0.5 * v2 - d3;

	// 0.375 * v1 - 0.5 * v1 = 0.5 * v2 - d3 - 0.125 * v2 + d2;
	// 0.375 * v1 - 0.5 * v1 = 0.5 * v2 - d3 - 0.125 * v2 + d2;
	// -0.125 * v1 = 0.375 * v2 - d3 + d2;
	// v1 = (0.375 * v2 - d3 + d2) / -0.125;

	// -3 * (0.375 * v2 - d3 + d2) + 0.125 * v2 - d2 = 0
	// -3 * (0.375 * v2 - d3 + d2) + 0.125 * v2 - d2 = 0
	// -1 * v2 + 3 * d3 - 3 * d2 - d2 = 0
	// v2 = 3 * d3 - 4 * d2

	// 0.5 * v1 + 0.5 * v2 - d3
	// -4 * (0.375 * v2 - d3 + d2) + 0.5 * v2 - d3 = 0
	// -1.5 * v2 + 4 * d3 - 4 * d2 + 0.5 * v2 - d3 = 0
	// v2 = 4 * d3 - 4 * d2 - d3 
	// v2 = 3 * d3 - 4 * d2

	// 0.5 * v1 + 0.5 * (3 * d3 - 4 * d2) - d3 = 0
	// v1 + (3 * d3 - 4 * d2) - 2 * d3 = 0
	// v1 = -3 * d3 + 4 * d2 + 2 * d3
	// v1 = -1 * d3 + 4 * d2



void ConvertToAnimLocal( s_animation_t *panim, Vector &pos, QAngle &angles )
{
	matrix3x4_t bonematrix;
	matrix3x4_t adjmatrix;

	// convert explicit position/angle into animation relative values
	AngleMatrix( angles, pos, bonematrix );
	AngleMatrix( panim->rotation, panim->adjust, adjmatrix );
	MatrixInvert( adjmatrix, adjmatrix );
	ConcatTransforms( adjmatrix, bonematrix, bonematrix );
	MatrixAngles( bonematrix, angles, pos );
	// pos = pos * panim->scale;
}

//-----------------------------------------------------------------------------
// Purpose: find the linear movement/rotation between two frames, subtract that 
//			out of the animation and add it back on as a "piecewise movement" command
// panim		- current animation
// motiontype	- what to extract
// iStartFrame	- first frame to apply motion over
// iEndFrame	- last end frame to apply motion over
// iSrcFrame	- match refFrame against what frame of the current animation
// pRefAnim		- reference animtion
// iRefFrame	- frame of reference animation to match
//-----------------------------------------------------------------------------



void AxisAngleToQuaternion(const Vector& axis, float angle, Quaternion& outQ)
{
	Vector normAxis = axis;
	float length = normAxis.Length();
	if (length > 0.0001f)
	{
		normAxis /= length;
	}
	else
	{
		outQ.x = 0.0f;
		outQ.y = 0.0f;
		outQ.z = 0.0f;
		outQ.w = 1.0f;
		return;
	}

	float halfAngle = angle * 0.5f;
	float sinHalf = sinf(halfAngle);

	outQ.x = normAxis.x * sinHalf;
	outQ.y = normAxis.y * sinHalf;
	outQ.z = normAxis.z * sinHalf;
	outQ.w = cosf(halfAngle);
}

void QuaternionToAxisAngle(const Quaternion& q, Vector& outAxis, float& outAngle)
{
	Quaternion normQ = q;
	QuaternionNormalize(normQ);

	outAngle = 2.0f * acosf(normQ.w);

	if (outAngle < 0.0001f)
	{
		outAxis.x = 1.0f;
		outAxis.y = 0.0f;
		outAxis.z = 0.0f;
		outAngle = 0.0f;
		return;
	}

	float sinHalf = sinf(outAngle * 0.5f);
	if (fabs(sinHalf) > 0.0001f)
	{
		outAxis.x = normQ.x / sinHalf;
		outAxis.y = normQ.y / sinHalf;
		outAxis.z = normQ.z / sinHalf;
	}
	else
	{
		outAxis.x = 1.0f;
		outAxis.y = 0.0f;
		outAxis.z = 0.0f;
	}

	float length = outAxis.Length();
	if (length > 0.0001f)
	{
		outAxis /= length;
	}
	else
	{
		outAxis.x = 1.0f;
		outAxis.y = 0.0f;
		outAxis.z = 0.0f;
	}

	if (outAngle < 0.0f) outAngle += 2.0f * M_PI;
}

Quaternion QuaternionAxisProject(const Quaternion& q, const Vector& axis)
{
	Vector qAxis;
	float angle;


	//QuaternionNormalize(q);

	QuaternionToAxisAngle(q, qAxis, angle);

	// Проецируем ось вращения на целевую ось
	float projection = DotProduct(qAxis, axis);
	Vector projAxis = axis * projection;
	if (projAxis.LengthSqr() > 0)
		VectorNormalize(projAxis);
	//projAxis.Normalize();

// Создаём новый кватернион с проекцией
	Quaternion result;
	AxisAngleToQuaternion(projAxis, angle * fabs(projection), result);
	return result;
}

void extractLinearMotion( s_animation_t *panim, int motiontype, int iStartFrame, int iEndFrame, int iSrcFrame, s_animation_t *pRefAnim, int iRefFrame /* , Vector pos, QAngle angles */ )
{
	int j, k;
	matrix3x4_t adjmatrix;

	// Can't extract motion with only 1 frame of animation!
	if ( panim->numframes <= 1 )
	{
		MdlError( "Can't extract motion from sequence %s (%s).  Check your " SRC_FILE_EXT " options!\n", panim->name, panim->filename );
	}

	if (panim->numpiecewisekeys >= MAXSTUDIOMOVEKEYS)
	{
		MdlError( "Too many piecewise movement keys in %s (%s)\n", panim->name, panim->filename );
	}

	if (iEndFrame > panim->numframes - 1)
		iEndFrame = panim->numframes - 1;

	if (iSrcFrame > panim->numframes - 1)
		iSrcFrame = panim->numframes - 1;

	if (iStartFrame >= iEndFrame)
	{
		MdlWarning("Motion extraction ignored, no frames remaining in %s (%s)\n", panim->name, panim->filename );
		return;
	}

	MdlWarning("Trying to use deprecated extractLinearMotion\n");

	float fFrame = (iStartFrame + iSrcFrame) / 2.0;
	int iMidFrame = (int)fFrame;
	float s = fFrame - iMidFrame;

	// find rotation
	RadianEuler	rot( 0, 0, 0 );

	int iRootIndex = g_rootIndex;

	int iCustomRootMotionBoneIndex = findGlobalBone( rootname );
	if ( iCustomRootMotionBoneIndex != -1 )
	{
		iRootIndex = iCustomRootMotionBoneIndex;
	}

	if (motiontype & (STUDIO_LXR | STUDIO_LYR | STUDIO_LZR))
	{
		Quaternion q0 = pRefAnim->sanim[iRefFrame][iRootIndex].qrot; // Используем qrot
		Quaternion q1 = panim->sanim[iMidFrame][iRootIndex].qrot;
		Quaternion q2 = panim->sanim[iSrcFrame][iRootIndex].qrot;

		Quaternion deltaQ1, deltaQ2;
		QuaternionMA(q1, -1, q0, deltaQ1); // deltaQ1 = q1 * q0⁻¹
		QuaternionMA(q2, -1, q0, deltaQ2); // deltaQ2 = q2 * q0⁻¹

		Quaternion finalQ = quat_identity;

		QuaternionNormalize(deltaQ1);
		QuaternionNormalize(deltaQ2);
		QuaternionNormalize(finalQ);



		// Probably this replacements are REAL shit? IDK. (2025)
		if (motiontype & STUDIO_LXR)
		{

			Quaternion qX = QuaternionAxisProject(deltaQ2, Vector(1, 0, 0));
			finalQ = finalQ * qX;
		}
		if (motiontype & STUDIO_LYR)
		{
			Quaternion qY = QuaternionAxisProject(deltaQ2, Vector(0, 1, 0));
			finalQ = finalQ * qY;
		}
		if (motiontype & STUDIO_LZR)
		{			
			Quaternion qZ = QuaternionAxisProject(deltaQ2, Vector(0, 0, 1));
			Quaternion qMid = QuaternionAxisProject(deltaQ1, Vector(0, 0, 1));
			
			if (qMid.w < 0) {
				QuaternionNormalize(qMid);
			}
			
			finalQ = finalQ * qZ;
		}

		// Построение матрицы из кватерниона
		matrix3x4_t adjmatrix;
		QuaternionMatrix(finalQ, adjmatrix);

		// Применение вращения к позиции
			
	}

	// find movement
	Vector p0;
	VectorRotate( pRefAnim->sanim[iRefFrame][iRootIndex].pos, adjmatrix, p0 );

	Vector p2 = panim->sanim[iSrcFrame][iRootIndex].pos;
	Vector p1 = panim->sanim[iMidFrame][iRootIndex].pos * (1 - s) + panim->sanim[iMidFrame+1][iRootIndex].pos * s;

	// ConvertToAnimLocal( panim, pos, angles ); // FIXME: unused

	p2 = p2 - p0;
	p1 = p1 - p0;

	if (!(motiontype & STUDIO_LX)) { p2.x = 0; p1.x = 0; };
	if (!(motiontype & STUDIO_LY)) { p2.y = 0; p1.y = 0; };
	if (!(motiontype & STUDIO_LZ)) { p2.z = 0; p1.z = 0; };
	
	// printf("%s  %.1f %.1f %.1f\n", g_bonetable[iRootIndex].name, p2.x, p2.y, p2.z );

	float d1 = p1.Length();
	float d2 = p2.Length();

	float v0 = -1 * d2 + 4 * d1;
	float v1 = 3 * d2 - 4 * d1;

	if ( g_verbose )
	{
		printf("%s : %d - %d : %.1f %.1f %.1f root: %s\n", panim->name, iStartFrame, iEndFrame, p2.x, p2.y, RAD2DEG( rot[2] ), g_bonetable[iRootIndex].name );
	}

	int numframes = iEndFrame - iStartFrame + 1;
	if (numframes < 1)
		return;

	float n = numframes - 1;

	//printf("%f %f : ", v0, v1 );

	if (motiontype & STUDIO_LINEAR)
	{
		v0 = v1 = p2.Length();
	}
	else if (v0 < 0.0f)
	{
		v0 = 0.0;
		v1 = p2.Length() * 2.0;
	}
	else if (v1 < 0.0)
	{
		v0 = p2.Length() * 2.0;
		v1 = 0.0;
	}
	else if ((v0+v1) > 0.01 && (fabs(v0-v1) / (v0+v1)) < 0.2)
	{
		// if they're within 10% of each other, assum no acceleration
		v0 = v1 = p2.Length();
	}

	//printf("%f %f\n", v0, v1 );

	Vector v = p2;
	VectorNormalize( v );

	Vector A, B, C;
	if (motiontype & STUDIO_QUADRATIC_MOTION)
	{
		SolveInverseQuadratic( 0, 0, 0.5, p1.x, 1.0, p2.x, A.x, B.x, C.x );
		SolveInverseQuadratic( 0, 0, 0.5, p1.y, 1.0, p2.y, A.y, B.y, C.y );
		SolveInverseQuadratic( 0, 0, 0.5, p1.z, 1.0, p2.z, A.z, B.z, C.z );
	}

	Vector	adjpos;
	
	// 2025 idk 
	Quaternion adjquat;
	MatrixQuaternion(adjmatrix, adjquat);

	matrix3x4_t bonematrix;
	for (j = 0; j < numframes; j++)
	{	
		float t = (j / n);

		if (motiontype & STUDIO_QUADRATIC_MOTION)
		{
			adjpos.x = t * t * A.x + t * B.x + C.x;
			adjpos.y = t * t * A.y + t * B.y + C.y;
			adjpos.z = t * t * A.z + t * B.z + C.z;
		}
		else
		{
			VectorScale( v, v0 * t + 0.5 * (v1 - v0) * t * t, adjpos );
		}


		QuaternionScale(adjquat, t, adjquat);
		QuaternionMatrix(adjquat, adjpos, adjmatrix );
		MatrixInvert( adjmatrix, adjmatrix );

		for (k = 0; k < g_numbones; k++)
		{
			if (g_bonetable[k].parent == -1)
			{
				// printf(" %.1f %.1f %.1f : ", adjpos[0], adjpos[1], RAD2DEG( adjangle[2] ));

				// printf(" %.1f %.1f %.1f\n", adjpos[0], adjpos[1], adjpos[2] );

				QuaternionMatrix( panim->sanim[j+iStartFrame][k].qrot, panim->sanim[j+iStartFrame][k].pos, bonematrix );
				ConcatTransforms( adjmatrix, bonematrix, bonematrix );

				MatrixQuaternion( bonematrix, panim->sanim[j+iStartFrame][k].qrot, panim->sanim[j+iStartFrame][k].pos );
				// printf("%d : %.1f %.1f %.1f\n", j, panim->sanim[j+iStartFrame][k].pos.x, panim->sanim[j+iStartFrame][k].pos.y, RAD2DEG( panim->sanim[j+iStartFrame][k].rot.z ) );
			}
		}
	}

	for (; j+iStartFrame < panim->numframes; j++)
	{
		for (k = 0; k < g_numbones; k++)
		{
			if (g_bonetable[k].parent == -1)
			{
				QuaternionMatrix( panim->sanim[j+iStartFrame][k].qrot, panim->sanim[j+iStartFrame][k].pos, bonematrix );
				ConcatTransforms( adjmatrix, bonematrix, bonematrix );
				MatrixQuaternion( bonematrix, panim->sanim[j+iStartFrame][k].qrot, panim->sanim[j+iStartFrame][k].pos );
			}
		}
	}

	// create piecewise motion paths

	s_linearmove_t *pmove = &panim->piecewisemove[panim->numpiecewisekeys++];

	pmove->endframe = iEndFrame;

	pmove->flags = motiontype;

	// concatinate xforms
	if (panim->numpiecewisekeys > 1)
	{
		QuaternionMatrix(adjquat, adjpos, bonematrix);
		QuaternionMatrix( pmove[-1].qrot, pmove[-1].pos, adjmatrix );
		ConcatTransforms( adjmatrix, bonematrix, bonematrix );
		MatrixQuaternion( bonematrix, pmove[0].qrot, pmove[0].pos );
		pmove->vector = pmove[0].pos - pmove[-1].pos;
	}
	else
	{
		VectorCopy( adjpos, pmove[0].pos );
		QuaternionCopy(adjquat, pmove[0].qrot );
		pmove->vector = pmove[0].pos;
	}
	VectorNormalize( pmove->vector );

	// printf("%d : %.1f %.1f %.1f\n", iEndFrame, pmove[0].pos.x, pmove[0].pos.y, RAD2DEG( pmove[0].rot.z ) ); 

	pmove->v0 = v0;
	pmove->v1 = v1;
}



//-----------------------------------------------------------------------------
// Purpose: process the "piecewise movement" commands and return where the animation
//			would move to on a given frame (assuming frame 0 is at the origin)
//-----------------------------------------------------------------------------

Vector calcPosition( s_animation_t *panim, int iFrame )
{
	Vector vecPos;
	
	vecPos.Init();

	if (panim->numpiecewisekeys == 0)
		return vecPos;

	if (panim->numframes == 1)
		return vecPos;

	int iLoops = 0;
	while (iFrame >= (panim->numframes - 1))
	{
		iLoops++;
		iFrame = iFrame - (panim->numframes - 1);
	}

	float	prevframe = 0.0f;

	for (int i = 0; i < panim->numpiecewisekeys; i++)
	{
		s_linearmove_t *pmove = &panim->piecewisemove[i];

		if (pmove->endframe >= iFrame)
		{
			float f = (iFrame - prevframe) / (pmove->endframe - prevframe);

			float d = pmove->v0 * f + 0.5 * (pmove->v1 - pmove->v0) * f * f;

			vecPos = vecPos + d * pmove->vector;
			if (iLoops != 0)
			{
				s_linearmove_t *pmove = &panim->piecewisemove[panim->numpiecewisekeys - 1];
				vecPos = vecPos + iLoops * pmove->pos; 
			}
			return vecPos;
		}
		else
		{
			prevframe = pmove->endframe;
			vecPos = pmove->pos;
		}
	}
	return vecPos;
}


//-----------------------------------------------------------------------------
// Purpose: calculate how far an animation travels between two frames
//-----------------------------------------------------------------------------

Vector calcMovement( s_animation_t *panim, int iFrom, int iTo )
{
	Vector p1 = calcPosition( panim, iFrom );
	Vector p2 = calcPosition( panim, iTo );

	return p2 - p1;
}

#if 0
	// FIXME: add in correct motion!!!
	int iFrame = pRule->peak - pRule->start - k;
	if (pRule->start + k > panim->numframes - 1)
	{
		iFrame = iFrame + 1;
	}
	Vector pos = footfall;
	if (panim->numframes > 1)
		pos = pos + panim->piecewisemove[0].pos * (iFrame) / (panim->numframes - 1.0f);
#endif

	
//-----------------------------------------------------------------------------
// Purpose: try to calculate a "missing" frame of animation, i.e the overlapping frame
//-----------------------------------------------------------------------------

void fixupMissingFrame( s_animation_t *panim )
{
	// the animations DIDN'T have the end frame the same as the start frame, so fudge it
	int size = g_numbones * sizeof( s_bone_t );
	int j = panim->numframes;

	float scale = 1 / (j - 1.0f);

	panim->sanim[j] = (s_bone_t *)calloc( 1, size );

	Vector deltapos;

	for (int k = 0; k < g_numbones; k++)
	{
		VectorSubtract( panim->sanim[j-1][k].pos, panim->sanim[0][k].pos, deltapos );
		VectorMA( panim->sanim[j-1][k].pos, scale, deltapos, panim->sanim[j][k].pos );
		QuaternionCopy( panim->sanim[0][k].qrot, panim->sanim[j][k].qrot );
	}

	panim->numframes = j + 1;
}


//-----------------------------------------------------------------------------
// Purpose: shift the frames of the animation so that it starts on the desired frame
//-----------------------------------------------------------------------------

void realignLooping( s_animation_t *panim )
{	
	int j, k;

	// realign looping animations
	if (panim->numframes > 1 && ( panim->looprestart != 0 || panim->looprestartpercent != 0 ) )
	{

		if ( panim->looprestartpercent != 0 )
		{
			panim->looprestart = (int)((panim->looprestartpercent / 100.0f) * (float)panim->numframes);
		}

		if ( panim->looprestart < 0 )
		{
			panim->looprestart += panim->numframes;
		}

		if ( panim->looprestart >= panim->numframes )
		{
			panim->looprestart -= panim->numframes;
		}

		if ( panim->looprestart < 0 || panim->looprestart >= panim->numframes )
		{
			MdlError( "loopstart (%d) out of range for animation %s (%d)", panim->looprestart, panim->name, panim->numframes );
		}

		for (k = 0; k < g_numbones; k++)
		{
			int n;

			Vector	shiftpos[MAXSTUDIOANIMFRAMES];
			Quaternion shiftrot[MAXSTUDIOANIMFRAMES];

			// printf("%f %f %f\n", motion[0], motion[1], motion[2] );
			for (j = 0; j < panim->numframes - 1; j++)
			{	
				n = (j + panim->looprestart) % (panim->numframes - 1);
				VectorCopy( panim->sanim[n][k].pos, shiftpos[j] );
				QuaternionCopy( panim->sanim[n][k].qrot, shiftrot[j] );
			}

			n = panim->looprestart;
			j = panim->numframes - 1;
			VectorCopy( panim->sanim[n][k].pos, shiftpos[j] );
			QuaternionCopy( panim->sanim[n][k].qrot, shiftrot[j] );

			for (j = 0; j < panim->numframes; j++)
			{	
				VectorCopy( shiftpos[j], panim->sanim[j][k].pos );
				QuaternionCopy( shiftrot[j], panim->sanim[j][k].qrot );
			}
		}
	}
}

void extractUnusedMotion( s_animation_t *panim )
{
	MdlWarning("Trying to use deprecated extractUnusedMotion (Disabled)\n");

	/*
	int j, k;

	int type = panim->motiontype;

	for (k = 0; k < g_numbones; k++)
	{
		if (g_bonetable[k].parent == -1)
		{
			float	motion[7];
			motion[0] = panim->sanim[0][k].pos[0];
			motion[1] = panim->sanim[0][k].pos[1];
			motion[2] = panim->sanim[0][k].pos[2];
			motion[3] = panim->sanim[0][k].rot[0];
			motion[4] = panim->sanim[0][k].rot[1];
			motion[5] = panim->sanim[0][k].rot[2];
			motion[6] = panim->sanim[0][k].rot[3];

			for (j = 0; j < panim->numframes; j++)
			{	
				if (type & STUDIO_X)
					panim->sanim[j][k].pos[0] = motion[0];
				if (type & STUDIO_Y)
					panim->sanim[j][k].pos[1] = motion[1];
				if (type & STUDIO_Z)
					panim->sanim[j][k].pos[2] = motion[2];
				if (type & STUDIO_XR)
					panim->sanim[j][k].rot[0] = motion[3];
				if (type & STUDIO_YR)
					panim->sanim[j][k].rot[1] = motion[4];
				if (type & STUDIO_ZR)
					panim->sanim[j][k].rot[2] = motion[5];
			}
		}
	}
	*/
}

//-----------------------------------------------------------------------------
// Purpose: find the difference between the src and dest animations, then add that
//			difference to all the frames of the dest animation.
//-----------------------------------------------------------------------------

void processMatch( s_animation_t *psrc, s_animation_t *pdest, int flags )
{
	int j, k;

	// process "match"
	Vector delta_pos[MAXSTUDIOSRCBONES];
	Quaternion delta_q[MAXSTUDIOSRCBONES];

	for (k = 0; k < g_numbones; k++)
	{
		if (flags)
			VectorSubtract( psrc->sanim[0][k].pos, pdest->sanim[0][k].pos, delta_pos[k] );
		QuaternionSM( -1, pdest->sanim[0][k].qrot, psrc->sanim[0][k].qrot, delta_q[k] );
	}

	// printf("%.2f %.2f %.2f\n", adj.x, adj.y, adj.z );
	for (j = 0; j < pdest->numframes; j++)
	{
		for (k = 0; k < g_numbones; k++)
		{
			if (pdest->weight[k] > 0)
			{
				if (flags)
					VectorAdd( pdest->sanim[j][k].pos, delta_pos[k], pdest->sanim[j][k].pos );
				QuaternionMA( pdest->sanim[j][k].qrot, 1.0, delta_q[k], pdest->sanim[j][k].qrot );
			}
		}	
	}
}


//-----------------------------------------------------------------------------
// Purpose: blend the psrc animation overtop the pdest animation, but blend the 
//			quaternions in world space instead of parent bone space.
//			Also, blend bone lengths, but only for non root animations.
//-----------------------------------------------------------------------------

void worldspaceBlend( s_animation_t *psrc, s_animation_t *pdest, int srcframe, int flags )
{
	int j, k, n;

	// process "match"
	Quaternion srcQ[MAXSTUDIOSRCBONES];
	Vector srcPos[MAXSTUDIOSRCBONES];
	Vector tmp;

	matrix3x4_t srcBoneToWorld[MAXSTUDIOBONES];
	matrix3x4_t destBoneToWorld[MAXSTUDIOBONES];

	if (!flags)
	{
		CalcBoneTransforms( psrc, srcframe, srcBoneToWorld );
		for (k = 0; k < g_numbones; k++)
		{
			MatrixAngles( srcBoneToWorld[k], srcQ[k], tmp );
			srcPos[k] = psrc->sanim[srcframe][k].pos;
		}
	}

	Quaternion targetQ, destQ;

	// printf("%.2f %.2f %.2f\n", adj.x, adj.y, adj.z );
	for (j = 0; j < pdest->numframes; j++)
	{
		if (flags)
		{
			// pull from a looping source
			float flCycle = (float)j / (pdest->numframes - 1);
			flCycle += (float)srcframe / (psrc->numframes - 1);
			CalcBoneTransformsCycle( psrc, psrc, flCycle, srcBoneToWorld );
			for (k = 0; k < g_numbones; k++)
			{
				MatrixAngles( srcBoneToWorld[k], srcQ[k], tmp );

				n = g_bonetable[k].parent;
				if (n == -1)
				{
					MatrixPosition( srcBoneToWorld[k], srcPos[k] );
				}
				else
				{
					matrix3x4_t worldToBone;
					MatrixInvert( srcBoneToWorld[n], worldToBone );

					matrix3x4_t local;
					ConcatTransforms( worldToBone, srcBoneToWorld[k], local );
					MatrixPosition( local, srcPos[k] );
				}
			}
		}


		CalcBoneTransforms( pdest, j, destBoneToWorld );

		for (k = 0; k < g_numbones; k++)
		{
			if (pdest->weight[k] > 0)
			{
				// blend the boneToWorld transforms in world space
				MatrixAngles( destBoneToWorld[k], destQ, tmp );
				QuaternionSlerp( destQ, srcQ[k], pdest->weight[k], targetQ );

				AngleMatrix( RadianEuler( targetQ ), tmp, destBoneToWorld[k] );
			}

			// back solve
			n = g_bonetable[k].parent;
			if (n == -1)
			{
				MatrixQuaternion( destBoneToWorld[k], pdest->sanim[j][k].qrot, tmp );

				// FIXME: it's not clear if this should blend position or not....it'd be 
				// better if weight lists could do quat and pos independently. 
			}
			else
			{
				matrix3x4_t worldToBone;
				MatrixInvert( destBoneToWorld[n], worldToBone );

				matrix3x4_t local;
				ConcatTransforms( worldToBone, destBoneToWorld[k], local );
				MatrixQuaternion( local, pdest->sanim[j][k].qrot, tmp );

				// blend bone lengths (local space)
				pdest->sanim[j][k].pos = Lerp( pdest->posweight[k], pdest->sanim[j][k].pos, srcPos[k] );
			}
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: match one animations position/orientation to another animations position/orientation
//-----------------------------------------------------------------------------

void processAutoorigin( s_animation_t *psrc, s_animation_t *pdest, int motiontype, int srcframe, int destframe, int bone )
{	
	int j, k;
	matrix3x4_t adjmatrix;

	matrix3x4_t srcBoneToWorld[MAXSTUDIOBONES];
	matrix3x4_t destBoneToWorld[MAXSTUDIOBONES];

	CalcBoneTransforms( psrc, srcframe, srcBoneToWorld );
	CalcBoneTransforms( pdest, destframe, destBoneToWorld );

	// find rotation
	RadianEuler	rot( 0, 0, 0 );

	Quaternion q0;
	Quaternion q2;
	Vector srcPos;
	Vector destPos;

	MatrixAngles( srcBoneToWorld[bone], q0, srcPos );
	MatrixAngles( destBoneToWorld[bone], q2, destPos );

	if (motiontype & (STUDIO_LXR | STUDIO_LYR | STUDIO_LZR | STUDIO_XR | STUDIO_YR | STUDIO_ZR))
	{
		Quaternion deltaQ2;
		QuaternionMA( q2, -1, q0, deltaQ2 );

		RadianEuler a3;
		if (motiontype & (STUDIO_LXR | STUDIO_XR))
		{
			Quaternion q4;
			q4.Init( deltaQ2.x, 0, 0, deltaQ2.w );
			QuaternionNormalize( q4 );
			QuaternionAngles( q4, a3 );
			rot.x = a3.x;
		}
		if (motiontype & (STUDIO_LYR | STUDIO_YR))
		{
			Quaternion q4;
			q4.Init( 0, deltaQ2.y, 0, deltaQ2.w );
			QuaternionNormalize( q4 );
			QuaternionAngles( q4, a3 );
			rot.y = a3.y;
		}
		if (motiontype & (STUDIO_LZR | STUDIO_ZR))
		{
			Quaternion q4;
			q4.Init( 0, 0, deltaQ2.z, deltaQ2.w );
			QuaternionNormalize( q4 );
			QuaternionAngles( q4, a3 );
			rot.z = a3.z;
		}
		if ((motiontype & STUDIO_XR) && (motiontype & STUDIO_YR) && (motiontype & STUDIO_ZR))
		{
			QuaternionAngles( deltaQ2, rot );
		}
	}

	// find movement
	Vector p0 = srcPos;
	Vector p2;
	AngleMatrix(rot, adjmatrix );
	MatrixInvert( adjmatrix, adjmatrix );
	VectorRotate( destPos, adjmatrix, p2 );

	Vector adj = p0 - p2;

	if (!(motiontype & (STUDIO_X | STUDIO_LX)))
		adj.x = 0;
	if (!(motiontype & (STUDIO_Y | STUDIO_LY)))
		adj.y = 0;
	if (!(motiontype & (STUDIO_Z | STUDIO_LZ)))
		adj.z = 0;

	PositionMatrix( adj, adjmatrix );

	if (g_verbose && bone != g_rootIndex)
	{
		printf("%s aligning to %s - %.2f %.2f %.2f\n", pdest->name, g_bonetable[bone].name, adj.x, adj.y, adj.z );
	}

	for (k = 0; k < g_numbones; k++)
	{
		if (g_bonetable[k].parent == -1)
		{
			for (j = 0; j < pdest->numframes; j++)
			{
				matrix3x4_t bonematrix;
				QuaternionMatrix( pdest->sanim[j][k].qrot, pdest->sanim[j][k].pos, bonematrix );
				ConcatTransforms( adjmatrix, bonematrix, bonematrix );
				MatrixQuaternion( bonematrix, pdest->sanim[j][k].qrot, pdest->sanim[j][k].pos );
			}
		}
	}	
}

//-----------------------------------------------------------------------------
// Purpose: subtract one animaiton from animation to create an animation of the "difference"
//-----------------------------------------------------------------------------

void subtractBaseAnimations( s_animation_t *psrc, s_animation_t *pdest, int srcframe, int flags )
{
	int j, k;

 	// create delta animations
	s_bone_t src[MAXSTUDIOSRCBONES];

	if (srcframe >= psrc->numframes)
	{
		MdlError( "subtract frame %d out of range for %s\n", srcframe, psrc->name );
	}

	for (k = 0; k < g_numbones; k++)
	{
		VectorCopy( psrc->sanim[srcframe][k].pos, src[k].pos );
		QuaternionCopy( psrc->sanim[srcframe][k].qrot, src[k].qrot );
	}

	for (k = 0; k < g_numbones; k++)
	{
		for (j = 0; j < pdest->numframes; j++)
		{	
			if (pdest->weight[k] > 0)
			{
				/*
				printf("%2d : %7.2f %7.2f %7.2f  %7.2f %7.2f %7.2f\n", 
					k,
					src[k].pos[0], src[k].pos[1], src[k].pos[2], 
					src[k].rot[0], src[k].rot[1], src[k].rot[2] ); 

				printf("     %7.2f %7.2f %7.2f  %7.2f %7.2f %7.2f\n", 
					RAD2DEG(pdest->sanim[j][k].pos[0]), RAD2DEG(pdest->sanim[j][k].pos[1]), RAD2DEG(pdest->sanim[j][k].pos[2]), 
					RAD2DEG(pdest->sanim[j][k].rot[0]), RAD2DEG(pdest->sanim[j][k].rot[1]), RAD2DEG(pdest->sanim[j][k].rot[2]) ); 
				*/

				// calc differences between two rotations
				if (flags & STUDIO_POST)
				{
					// find pdest in src's reference frame  
					QuaternionSM( -1, src[k].qrot, pdest->sanim[j][k].qrot, pdest->sanim[j][k].qrot );
					VectorSubtract( pdest->sanim[j][k].pos, src[k].pos, pdest->sanim[j][k].pos );
				}
				else
				{
					// find src in pdest's reference frame?
					QuaternionMA( pdest->sanim[j][k].qrot, -1, src[k].qrot, pdest->sanim[j][k].qrot );
					VectorSubtract( src[k].pos, pdest->sanim[j][k].pos, pdest->sanim[j][k].pos );
				}

				/*
				printf("     %7.2f %7.2f %7.2f  %7.2f %7.2f %7.2f\n", 
					pdest->sanim[j][k].pos[0], pdest->sanim[j][k].pos[1], pdest->sanim[j][k].pos[2], 
					RAD2DEG(pdest->sanim[j][k].rot[0]), RAD2DEG(pdest->sanim[j][k].rot[1]), RAD2DEG(pdest->sanim[j][k].rot[2]) ); 
				*/
			}
		}
	}

#if 0
	// cleanup weightlists
	for (k = 0; k < g_numbones; k++)
	{
		panim->weight[k] = 0.0;
	}

	for (k = 0; k < g_numbones; k++)
	{
		if (g_weightlist[panim->weightlist].weight[k] > 0.0)
		{
			for (j = 0; j < panim->numframes; j++)
			{	
				if (fabs(panim->sanim[j][k].pos[0]) > 0.001 || 
					fabs(panim->sanim[j][k].pos[1]) > 0.001 || 
					fabs(panim->sanim[j][k].pos[2]) > 0.001 || 
					fabs(panim->sanim[j][k].rot[0]) > 0.001 || 
					fabs(panim->sanim[j][k].rot[1]) > 0.001 || 
					fabs(panim->sanim[j][k].rot[2]) > 0.001)
				{
					panim->weight[k] = g_weightlist[panim->weightlist].weight[k];
					break;
				}
			}
		}
	}
#endif
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------

void QuaternionSlerp( const RadianEuler &r0, const RadianEuler &r1, float t, RadianEuler &r2 )
{
	Quaternion q0, q1, q2;
	AngleQuaternion( r0, q0 );
	AngleQuaternion( r1, q1 );
	QuaternionSlerp( q0, q1, t, q2 );
	QuaternionAngles( q2, r2 );
}


//-----------------------------------------------------------------------------
// Purpose: subtract each frame running interpolation of the first frame to the last frame
//-----------------------------------------------------------------------------

void linearDelta( s_animation_t *psrc, s_animation_t *pdest, int srcframe, int flags )
{
	int j, k;

 	// create delta animations
	s_bone_t src0[MAXSTUDIOSRCBONES];
	s_bone_t src1[MAXSTUDIOSRCBONES];

	for (k = 0; k < g_numbones; k++)
	{
		VectorCopy( psrc->sanim[0][k].pos, src0[k].pos );
		QuaternionCopy( psrc->sanim[0][k].qrot, src0[k].qrot );
		VectorCopy( psrc->sanim[srcframe][k].pos, src1[k].pos );
		QuaternionCopy( psrc->sanim[srcframe][k].qrot, src1[k].qrot );
	}

	if (pdest->numframes == 1)
	{
		MdlWarning( "%s too short for splinedelta\n", pdest->name );
	}

	for (k = 0; k < g_numbones; k++)
	{
		for (j = 0; j < pdest->numframes; j++)
		{	
			float s = 1;
			if (pdest->numframes > 1)
			{
				s = (float)j / (pdest->numframes - 1);
			}

			// make it a spline curve
			if (flags & STUDIO_AL_SPLINE)
			{
				s = 3 * s * s - 2 * s * s * s;
			}

			if (pdest->weight[k] > 0)
			{
				/*
				printf("%2d : %7.2f %7.2f %7.2f  %7.2f %7.2f %7.2f\n", 
					k,
					src[k].pos[0], src[k].pos[1], src[k].pos[2], 
					src[k].rot[0], src[k].rot[1], src[k].rot[2] ); 

				printf("     %7.2f %7.2f %7.2f  %7.2f %7.2f %7.2f\n", 
					RAD2DEG(pdest->sanim[j][k].pos[0]), RAD2DEG(pdest->sanim[j][k].pos[1]), RAD2DEG(pdest->sanim[j][k].pos[2]), 
					RAD2DEG(pdest->sanim[j][k].rot[0]), RAD2DEG(pdest->sanim[j][k].rot[1]), RAD2DEG(pdest->sanim[j][k].rot[2]) ); 
				*/

				s_bone_t src;

				src.pos = src0[k].pos * (1 - s) + src1[k].pos * s;
				QuaternionSlerp( src0[k].qrot, src1[k].qrot, s, src.qrot );

				// calc differences between two rotations
				if (flags & STUDIO_AL_POST)
				{
					// find pdest in src's reference frame  
					QuaternionSM( -1, src.qrot, pdest->sanim[j][k].qrot, pdest->sanim[j][k].qrot );
					VectorSubtract( pdest->sanim[j][k].pos, src.pos, pdest->sanim[j][k].pos );
				}
				else
				{
					// find src in pdest's reference frame?
					QuaternionMA( pdest->sanim[j][k].qrot, -1, src.qrot, pdest->sanim[j][k].qrot );
					VectorSubtract( src.pos, pdest->sanim[j][k].pos, pdest->sanim[j][k].pos );
				}

				/*
				printf("     %7.2f %7.2f %7.2f  %7.2f %7.2f %7.2f\n", 
					pdest->sanim[j][k].pos[0], pdest->sanim[j][k].pos[1], pdest->sanim[j][k].pos[2], 
					RAD2DEG(pdest->sanim[j][k].rot[0]), RAD2DEG(pdest->sanim[j][k].rot[1]), RAD2DEG(pdest->sanim[j][k].rot[2]) ); 
				*/
			}
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: turn the animation into a lower fps encoded version
//-----------------------------------------------------------------------------

void reencodeAnimation( s_animation_t *panim, int frameskip )
{
	int j, k, n;

	n = 1;
	for (j = frameskip; j < panim->numframes; j += frameskip)
	{	
		for (k = 0; k < g_numbones; k++)
		{
			panim->sanim[n][k] = panim->sanim[j][k];
		}
		n++;
	}
	panim->numframes = n;

	panim->fps = panim->fps / frameskip;
}


//-----------------------------------------------------------------------------
// Purpose: clip or pad the animation as nessesary to be a specified number of frames
//-----------------------------------------------------------------------------

void forceNumframes( s_animation_t *panim, int numframes )
{
	int j;

	int size = g_numbones * sizeof( s_bone_t );

	// copy
	for (j = panim->numframes; j < numframes; j++)
	{	
		panim->sanim[j] = (s_bone_t *)calloc( 1, size );
		memcpy( panim->sanim[j], panim->sanim[panim->numframes-1], size );
	}

	panim->numframes = numframes;
}


//-----------------------------------------------------------------------------
// Purpose: subtract each frame from the previous to calculate the animations derivative
//-----------------------------------------------------------------------------

void createDerivative( s_animation_t *panim, float scale )
{
	int j, k;

	s_bone_t orig[MAXSTUDIOSRCBONES];

	j = panim->numframes - 1;
	if (panim->flags & STUDIO_LOOPING)
	{
		j--;
	}

	for (k = 0; k < g_numbones; k++)
	{
		VectorCopy( panim->sanim[j][k].pos, orig[k].pos );
		QuaternionCopy( panim->sanim[j][k].qrot, orig[k].qrot );
	}

	for (j = panim->numframes - 1; j >= 0; j--)
	{	
		s_bone_t *psrc;
		s_bone_t *pdest;

		if (j - 1 >= 0)
		{
			psrc = panim->sanim[j-1];
		}
		else
		{
			psrc = orig;
		}
		pdest = panim->sanim[j];

		for (k = 0; k < g_numbones; k++)
		{
			if (panim->weight[k] > 0)
			{
				/*
				{
					printf("%2d : %7.2f %7.2f %7.2f  %7.2f %7.2f %7.2f\n", 
						k,
						psrc[k].pos[0], psrc[k].pos[1], psrc[k].pos[2], 
						RAD2DEG(psrc[k].rot[0]), RAD2DEG(psrc[k].rot[1]), RAD2DEG(psrc[k].rot[2]) );

					printf("     %7.2f %7.2f %7.2f  %7.2f %7.2f %7.2f\n", 
						pdest[k].pos[0], pdest[k].pos[1], pdest[k].pos[2], 
						RAD2DEG(pdest[k].rot[0]), RAD2DEG(pdest[k].rot[1]), RAD2DEG(pdest[k].rot[2]) );
				}
				*/

				// find pdest in src's reference frame  
				QuaternionSM( -1, psrc[k].qrot, pdest[k].qrot, pdest[k].qrot );
				VectorSubtract( pdest[k].pos, psrc[k].pos, pdest[k].pos );

				// rescale results (not sure what basis physics system is expecting)
				{
					// QuaternionScale( pdest[k].rot, scale, pdest[k].rot );
					Quaternion q;
					
					QuaternionCopy( pdest[k].qrot, q );
					QuaternionScale( q, scale, q );					
					QuaternionCopy( q, pdest[k].qrot );

					VectorScale( pdest[k].pos, scale, pdest[k].pos );
				}

				/*
				{
					printf("     %7.2f %7.2f %7.2f  %7.2f %7.2f %7.2f\n", 
						pdest[k].pos[0], pdest[k].pos[1], pdest[k].pos[2], 
						RAD2DEG(pdest[k].rot[0]), RAD2DEG(pdest[k].rot[1]), RAD2DEG(pdest[k].rot[2]) ); 
				}
				*/
			}
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: subtract each frame from the previous to calculate the animations derivative
//-----------------------------------------------------------------------------

void clearAnimations( s_animation_t *panim, bool bRetainDuration )
{
	panim->flags |= STUDIO_DELTA;
	panim->flags |= STUDIO_ALLZEROS;
	
	if ( !bRetainDuration )
	{
		panim->numframes = 1;
		panim->startframe = 0;
		panim->endframe = 1;
	
		int k;

		for (k = 0; k < g_numbones; k++)
		{
			panim->sanim[0][k].pos = Vector( 0, 0, 0 );
			panim->sanim[0][k].qrot = Quaternion(0, 0, 0, 1);
			panim->weight[k] = 0.0;
			panim->posweight[k] = 0.0;
		}
	}
	else
	{
		// fixme: zero the bone data?
	}
}


//-----------------------------------------------------------------------------
// Purpose: remove all world rotation from a bone
//-----------------------------------------------------------------------------

void counterRotateBone( s_animation_t *panim, int iBone, QAngle target )
{
	matrix3x4_t boneToWorld[MAXSTUDIOBONES];
	Vector pos;
	matrix3x4_t defaultBoneToWorld;

	int j;

	AngleMatrix( target, defaultBoneToWorld );

	for (j = 0; j < panim->numframes; j++)
	{
		CalcBoneTransforms( panim, j, boneToWorld );

		MatrixPosition( boneToWorld[iBone], pos );
		PositionMatrix( pos, defaultBoneToWorld );
		boneToWorld[iBone] = defaultBoneToWorld;

		solveBone( panim, j, iBone, boneToWorld );
	}
}


//-----------------------------------------------------------------------------
// Purpose: build transforms in source space, assuming source bones
//-----------------------------------------------------------------------------
void BuildRawTransforms( const s_source_t *psource, const char *pAnimationName, 
	int frame, float scale, Vector const &shift, RadianEuler const &rotate, int flags, matrix3x4_t* boneToWorld )
{
	int k;
	Vector tmp;
	Vector pos;
	Quaternion qrot;
	matrix3x4_t bonematrix;
	
	matrix3x4_t rootxform;

	AngleMatrix( rotate, rootxform );

	const s_sourceanim_t *pSourceAnim = FindSourceAnim( psource, pAnimationName );
	if ( !pSourceAnim )
	{
		MdlError( "Unknown animation name %s\n", pAnimationName );
		return;
	}

	if ( flags & STUDIO_LOOPING )
	{
		if ( frame )
		{
			while ( frame < 0)
				frame += pSourceAnim->numframes;
			frame = frame % pSourceAnim->numframes;
		}
	}
	else
	{
		frame = clamp( frame, 0, pSourceAnim->numframes - 1 );
	}

	// build source space local to world transforms
	for (k = 0; k < psource->numbones; k++)
	{
		VectorScale( pSourceAnim->rawanim.Element(frame)[k].pos, scale, pos );
		QuaternionCopy( pSourceAnim->rawanim.Element(frame)[k].qrot, qrot );

		if ( psource->localBone[k].parent == -1 )
		{
			// translate
			VectorSubtract( pos, shift, tmp );

			// rotate
			VectorRotate( tmp, rootxform, pos );

			matrix3x4_t m = QuaternionMatrix(qrot);
			ConcatTransforms( rootxform, m, bonematrix );
			MatrixQuaternion(bonematrix, qrot);			
		}


		QuaternionMatrix( qrot, pos, bonematrix );

		if ( psource->localBone[k].parent == -1 )
		{
			MatrixCopy( bonematrix, boneToWorld[k] );
		}
		else
		{
			ConcatTransforms( boneToWorld[psource->localBone[k].parent], bonematrix, boneToWorld[k] );
			// ConcatTransforms( worldToBone[psource->localBone[k].parent], boneToWorld[k], bonematrix );
			// B * C => A
			// C <= B-1 * A
		}
	}
}


void BuildRawTransforms( const s_source_t *psource, const char *pAnimationName, int frame, matrix3x4_t* boneToWorld )
{
	BuildRawTransforms( psource, pAnimationName, frame, 1.0f, Vector( 0, 0, 0 ), RadianEuler( 0, 0, 0 ), 0, boneToWorld );
}


//-----------------------------------------------------------------------------
// Purpose: convert source bone animation into global bone animation
//-----------------------------------------------------------------------------
void TranslateAnimations( const s_source_t *pSource, const matrix3x4_t *pSrcBoneToWorld, matrix3x4_t *pDestBoneToWorld )
{
	matrix3x4_t bonematrix;

	for (int k = 0; k < g_numbones; k++)
	{
		int q = pSource->boneGlobalToLocal[k];
		if ( q == -1 )
		{
			// unknown bone, copy over defaults
			if ( g_bonetable[k].parent >= 0 )
			{
				QuaternionMatrix( g_bonetable[k].qrot, g_bonetable[k].pos, bonematrix );
				ConcatTransforms( pDestBoneToWorld[g_bonetable[k].parent], bonematrix, pDestBoneToWorld[k] );
			}
			else
			{
				QuaternionMatrix( g_bonetable[k].qrot, g_bonetable[k].pos, pDestBoneToWorld[k] );
			}
		}
		else
		{
			ConcatTransforms( pSrcBoneToWorld[q], g_bonetable[k].srcRealign, pDestBoneToWorld[k] );
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: convert source bone animation into global bone animation
//-----------------------------------------------------------------------------
void ConvertAnimation( const s_source_t *psource, const char *pAnimationName, int frame, float scale, Vector const &shift, RadianEuler const &rotate, s_bone_t *dest )
{
	int k;
	matrix3x4_t srcBoneToWorld[MAXSTUDIOSRCBONES];
	//matrix3x4_t srcWorldToBone[MAXSTUDIOSRCBONES];
	matrix3x4_t destBoneToWorld[MAXSTUDIOSRCBONES];
	matrix3x4_t destWorldToBone[MAXSTUDIOSRCBONES];

	matrix3x4_t bonematrix;

	BuildRawTransforms( psource, pAnimationName, frame, scale, shift, rotate, 0, srcBoneToWorld );

	/*
	for (k = 0; k < psource->numbones; k++)
	{
		MatrixInvert( srcBoneToWorld[k], srcWorldToBone[k] );
	}
	*/

	TranslateAnimations( psource, srcBoneToWorld, destBoneToWorld );

	for (k = 0; k < g_numbones; k++)
	{
		MatrixInvert( destBoneToWorld[k], destWorldToBone[k] );
	}

	// convert source_space_local_to_world transforms to shared_space_local_to_world transforms
	for (k = 0; k < g_numbones; k++)
	{
		if (g_bonetable[k].parent == -1)
		{
			MatrixCopy( destBoneToWorld[k], bonematrix );
		}
		else
		{
			// convert my transform into parent relative space
			ConcatTransforms( destWorldToBone[g_bonetable[k].parent], destBoneToWorld[k], bonematrix );

			// printf("%s : %s\n", psource->localBone[q2].name, psource->localBone[q].name );

			// B * C => A
			// C <= B-1 * A
		}

		MatrixQuaternion( bonematrix, dest[k].qrot, dest[k].pos );
	}
}

//-----------------------------------------------------------------------------
// Purpose: copy the raw animation data from the source files into the individual animations
//-----------------------------------------------------------------------------
void RemapAnimations(void)
{
	int i, j;

	// copy source animations
	for (i = 0; i < g_numani; i++)
	{
		s_animation_t *panim = g_panimation[i];

		s_source_t *psource = panim->source;
		s_sourceanim_t *pSourceAnim = FindSourceAnim( psource, panim->animationname );
		int size = g_numbones * sizeof( s_bone_t );

		int n = panim->startframe - pSourceAnim->startframe;
		// printf("%s %d:%d\n", g_panimation[i]->filename, g_panimation[i]->startframe, pSourceAnim->startframe );
		for (j = 0; j < panim->numframes; j++)
		{
			panim->sanim[j] = (s_bone_t *)calloc( 1, size );

			ConvertAnimation( psource, panim->animationname, n + j, panim->scale, panim->adjust, panim->rotation, panim->sanim[j] );
		}
	}
}

void buildAnimationWeights()
{
	int i, j, k;

	// rlink animation weights
	for (i = 0; i < g_numweightlist; i++)
	{
		if (i == 0)
		{
			// initialize weights
			for (j = 0; j < g_numbones; j++)
			{
				if (g_bonetable[j].parent != -1)
				{
					// set child bones to uninitialized
					g_weightlist[i].weight[j] = -1;
				}
				else if (i == 0)
				{
					// set root bones to 1
					g_weightlist[i].weight[j] = 1;
					g_weightlist[i].posweight[j] = 1;
				}
			}
		}
		else
		{
			// initialize weights
			for (j = 0; j < g_numbones; j++)
			{
				if (g_bonetable[j].parent != -1)
				{
					// set child bones to uninitialized
					g_weightlist[i].weight[j] = g_weightlist[0].weight[j];
					g_weightlist[i].posweight[j] = g_weightlist[0].posweight[j];
				}
				else
				{
					// set root bones to 0
					g_weightlist[i].weight[j] = 0;
					g_weightlist[i].posweight[j] = 0;
				}
			}
		}

		// match up weights
		for (j = 0; j < g_weightlist[i].numbones; j++)
		{
			k = findGlobalBone( g_weightlist[i].bonename[j] );
			if (k == -1)
			{
				MdlWarning("unknown bone reference '%s' in weightlist '%s'\n", g_weightlist[i].bonename[j], g_weightlist[i].name );
			}
			else
			{
				g_weightlist[i].weight[k] = g_weightlist[i].boneweight[j];
				g_weightlist[i].posweight[k] = g_weightlist[i].boneposweight[j];
			}
		}
	}

	for (i = 0; i < g_numweightlist; i++)
	{
		// copy weights forward
		for (j = 0; j < g_numbones; j++)
		{
			if (g_weightlist[i].weight[j] < 0.0)
			{
				if (g_bonetable[j].parent != -1)
				{
					g_weightlist[i].weight[j] = g_weightlist[i].weight[g_bonetable[j].parent];
					g_weightlist[i].posweight[j] = g_weightlist[i].posweight[g_bonetable[j].parent];
				}
			}
		}
	}
}

void setAnimationWeight( s_animation_t *panim, int index )
{
	// copy weightlists to animations
	for (int k = 0; k < g_numbones; k++)
	{
		panim->weight[k] = g_weightlist[index].weight[k];
		panim->posweight[k] = g_weightlist[index].posweight[k];
	}
}

void addDeltas( s_animation_t *panim, int frame, float s, Vector delta_pos[], Quaternion delta_q[] )
{
	for (int k = 0; k < g_numbones; k++)
	{
		if (panim->weight[k] > 0)
		{
			QuaternionSM(s, delta_q[k], panim->sanim[frame][k].qrot, panim->sanim[frame][k].qrot);
			VectorMA( panim->sanim[frame][k].pos, s, delta_pos[k], panim->sanim[frame][k].pos );
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: find the difference between the overlapping frames and spread out
//			the difference over multiple frames.
// start:	negative number, specifies how far back from the end to start blending
// end:		positive number, specifies how many frames from the beginning to blend
//-----------------------------------------------------------------------------

void fixupLoopingDiscontinuities( s_animation_t *panim, int start, int end )
{
	int j, k, m, n;

	// fix C0 errors on looping animations
	m = panim->numframes - 1;

	Vector delta_pos[MAXSTUDIOSRCBONES];
	Quaternion delta_q[MAXSTUDIOSRCBONES];

	// skip if there's nothing to smooth
	if (m == 0)
		return;

	for (k = 0; k < g_numbones; k++)
	{
		VectorSubtract( panim->sanim[m][k].pos, panim->sanim[0][k].pos, delta_pos[k] );
		QuaternionMA( panim->sanim[m][k].qrot, -1, panim->sanim[0][k].qrot, delta_q[k] );
		
		
		// по какой-то причине принт ниже не используется. 
		/*QAngle ang;
		QuaternionAngles( delta_q[k], ang );*/
		// printf("%2d  %.1f %.1f %.1f\n", k, ang.x, ang.y, ang.z );
	}

	// HACK: skip fixup for motion that'll be matched with linear extraction
	// FIXME: remove when "global" extraction moved into normal ordered processing loop
	for (k = 0; k < g_numbones; k++)
	{
		if (g_bonetable[k].parent == -1)
		{
			if (panim->motiontype & STUDIO_LX)
				delta_pos[k].x = 0.0;
			if (panim->motiontype & STUDIO_LY)
				delta_pos[k].y = 0.0;
			if (panim->motiontype & STUDIO_LZ)
				delta_pos[k].z = 0.0;
			// FIXME: add rotation
		}
	}

	// make sure loop doesn't exceed animation length
	if (end-start > panim->numframes)
	{
		end = panim->numframes + start;
		if (end < 0)
		{
			end = 0;
			start = -(panim->numframes - 1);
		}
	}

	// FIXME: figure out S
	float s = 0;
	float nf = end - start;
	
	for (j = start + 1; j <= 0; j++)
	{	
		n = j - start;
		s = (n / nf);
		s = 3 * s * s - 2 * s * s * s;
		// printf("%d : %d (%lf)\n", m+j, n, -s );
		addDeltas( panim, m+j, -s, delta_pos, delta_q );
	}

	for (j = 0; j < end; j++)
	{	
		n = end - j;
		s = (n / nf);
		s = 3 * s * s - 2 * s * s * s;
		//printf("%d : %d (%lf)\n", j, n, s );
		addDeltas( panim, j, s, delta_pos, delta_q );
	}
}



void matchBlend( s_animation_t *pDestAnim, s_animation_t *pSrcAnimation, int iSrcFrame, int iDestFrame, int iPre, int iPost )
{
	int j, k;

	if (pDestAnim->flags & STUDIO_LOOPING)
	{
		iPre = MAX( iPre, -pDestAnim->numframes );
		iPost = MIN( iPost, pDestAnim->numframes );
	}
	else
	{
		iPre = MAX( iPre, -iDestFrame );
		iPost = MIN( iPost, pDestAnim->numframes - iDestFrame );
	}

	Vector delta_pos[MAXSTUDIOSRCBONES];
	Quaternion delta_q[MAXSTUDIOSRCBONES];

	for (k = 0; k < g_numbones; k++)
	{
		VectorSubtract( pSrcAnimation->sanim[iSrcFrame][k].pos, pDestAnim->sanim[iDestFrame][k].pos, delta_pos[k] );
		QuaternionMA( pSrcAnimation->sanim[iSrcFrame][k].qrot, -1, pDestAnim->sanim[iDestFrame][k].qrot, delta_q[k] );
		/*
		QAngle ang;
		QuaternionAngles( delta_q[k], ang );
		printf("%2d  %.1f %.1f %.1f\n", k, ang.x, ang.y, ang.z );
		*/
	}

	// HACK: skip fixup for motion that'll be matched with linear extraction
	// FIXME: remove when "global" extraction moved into normal ordered processing loop
	for (k = 0; k < g_numbones; k++)
	{
		if (g_bonetable[k].parent == -1)
		{
			if (pDestAnim->motiontype & STUDIO_LX)
				delta_pos[k].x = 0.0;
			if (pDestAnim->motiontype & STUDIO_LY)
				delta_pos[k].y = 0.0;
			if (pDestAnim->motiontype & STUDIO_LZ)
				delta_pos[k].z = 0.0;
			// FIXME: add rotation
		}
	}

	// FIXME: figure out S
	float s = 0;

	for (j = iPre; j <= iPost; j++)
	{	
		if (j < 0)
		{
			s = j / (float)(iPre-1);
		}
		else
		{
			s = j / (float)(iPost+1);
		}
		s = SimpleSpline( 1 - s );
		k = iDestFrame + j;
		if (k < 0)
		{
			k += (pDestAnim->numframes - 1);
		}
		else
		{
			k = k % (pDestAnim->numframes - 1);
		}
		//printf("%d : %d (%lf)\n", iDestFrame + j, k, s );
		addDeltas( pDestAnim, k, s, delta_pos, delta_q );
		// make sure final frame of a looping animation matches frame 0
		if ((pDestAnim->flags & STUDIO_LOOPING) && k == 0)
		{
			addDeltas( pDestAnim, pDestAnim->numframes - 1, s, delta_pos, delta_q );
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: copy the first frame overtop the last frame
//-----------------------------------------------------------------------------

void forceAnimationLoop( s_animation_t *panim )
{

	MdlWarning("Trying to use forceAnimationLoop (Disabled) \n");

	

	// force looping animations to be looping

	// этот код нам больше не нужен, т.к мы юзаем входные кватернионы
	/*
	* int k, m, n;
	if (panim->flags & STUDIO_LOOPING)
	{
		n = 0;
		m = panim->numframes - 1;

		for (k = 0; k < g_numbones; k++)
		{
			int type = panim->motiontype;

			if (!(type & STUDIO_LX))
				panim->sanim[m][k].pos[0] = panim->sanim[n][k].pos[0];
			if (!(type & STUDIO_LY))
				panim->sanim[m][k].pos[1] = panim->sanim[n][k].pos[1];
			if (!(type & STUDIO_LZ))
				panim->sanim[m][k].pos[2] = panim->sanim[n][k].pos[2];

			if (!(type & STUDIO_LXR))
				panim->sanim[m][k].rot[0] = panim->sanim[n][k].rot[0];
			if (!(type & STUDIO_LYR))
				panim->sanim[m][k].rot[1] = panim->sanim[n][k].rot[1];
			if (!(type & STUDIO_LZR))
				panim->sanim[m][k].rot[2] = panim->sanim[n][k].rot[2];
		}
	}
	*/

	// printf("\n");
}



//-----------------------------------------------------------------------------
// Purpose: calculate an single bones animation in a different parent's reference frame
//-----------------------------------------------------------------------------

void localHierarchy( s_animation_t *panim, char *pBonename, char *pParentname, int start, int peak, int tail, int end  )
{
	s_localhierarchy_t *pRule;

	pRule = &panim->localhierarchy[ panim->numlocalhierarchy ];
	panim->numlocalhierarchy++;

	pRule->start = start;
	pRule->peak = peak;
	pRule->tail = tail;
	pRule->end = end;

	if (pRule->start == 0 && pRule->peak == 0 && pRule->tail == 0 && pRule->end == 0)
	{
		pRule->tail = panim->numframes - 1;
		pRule->end = panim->numframes - 1;
	}

	if (pRule->start != -1 && pRule->peak == -1 && pRule->tail == -1 && pRule->end != -1)
	{
		pRule->peak = (pRule->start + pRule->end) / 2;
		pRule->tail = (pRule->start + pRule->end) / 2;
	}

	if (pRule->start != -1 && pRule->peak == -1 && pRule->tail != -1)
	{
		pRule->peak = (pRule->start + pRule->tail) / 2;
	}

	if (pRule->peak != -1 && pRule->tail == -1 && pRule->end != -1)
	{
		pRule->tail = (pRule->peak + pRule->end) / 2;
	}

	if (pRule->peak == -1)
	{
		pRule->start = 0;
		pRule->peak = 0;
	}

	if (pRule->tail == -1)
	{
		pRule->tail = panim->numframes - 1;
		pRule->end = panim->numframes - 1;
	}

	// check for wrapping
	if (pRule->peak < pRule->start)
	{
		pRule->peak += panim->numframes - 1;
	}
	if (pRule->tail < pRule->peak)
	{
		pRule->tail += panim->numframes - 1;
	}
	if (pRule->end < pRule->tail)
	{
		pRule->end += panim->numframes - 1;
	}


	pRule->localData.numerror = pRule->end - pRule->start + 1;
	if (pRule->end >= panim->numframes)
		pRule->localData.numerror = pRule->localData.numerror + 2;

	pRule->localData.pError = (s_streamdata_t *)calloc( pRule->localData.numerror, sizeof( s_streamdata_t ));

	matrix3x4_t boneToWorld[MAXSTUDIOBONES];
	matrix3x4_t worldToBone;
	matrix3x4_t local;

	pRule->bone = findGlobalBone( pBonename );
	if (pRule->bone == -1)
	{
		MdlError("anim '%s' references unknown bone '%s' in localhierarchy\n", panim->name, pBonename );
	}

	if (strlen( pParentname ) == 0)
	{
		pRule->newparent = -1;
	}
	else
	{
		pRule->newparent = findGlobalBone( pParentname );
		if (pRule->newparent == -1)
		{
			MdlError("anim '%s' references unknown bone '%s' in localhierarchy\n", panim->name, pParentname );
		}
	}

	int k;
	const char *pAnimationName = panim->animationname;
	s_sourceanim_t *pSourceAnim = FindSourceAnim( panim->source, pAnimationName );


	for (k = 0; k < pRule->localData.numerror; k++)
	{
		matrix3x4_t srcBoneToWorld[MAXSTUDIOSRCBONES];
		BuildRawTransforms( panim->source, pAnimationName, k + pRule->start + panim->startframe - pSourceAnim->startframe, panim->scale, panim->adjust, panim->rotation, panim->flags, srcBoneToWorld );

		TranslateAnimations( panim->source, srcBoneToWorld, boneToWorld );

		if (pRule->newparent != -1)
		{
			MatrixInvert( boneToWorld[pRule->newparent], worldToBone );
			ConcatTransforms( worldToBone, boneToWorld[pRule->bone], local );
		}
		else
		{
			MatrixCopy( boneToWorld[pRule->bone], local );
		}

		MatrixAngles( local, pRule->localData.pError[k].q, pRule->localData.pError[k].pos );

		/*
		QAngle ang;
		QuaternionAngles( pRule->errorData.pError[k].q, ang );
		printf("%d  %.1f %.1f %.1f : %.1f %.1f %.1f\n", 
			k,
			pRule->errorData.pError[k].pos.x, pRule->errorData.pError[k].pos.y, pRule->errorData.pError[k].pos.z, 
			ang.x, ang.y, ang.z );
		*/
	}
}



//-----------------------------------------------------------------------------
// Purpose: rotate the animation so that it's moving in the specified angle
//-----------------------------------------------------------------------------


void makeAngle( s_animation_t *panim, float angle )
{
	float da = 0.0f;

	if (panim->numpiecewisekeys != 0)
	{
		// look for movement in total piecewise movement
		Vector pos = panim->piecewisemove[panim->numpiecewisekeys-1].pos;
		if (pos[0] != 0 || pos[1] != 0)
		{
			float a = atan2( pos[1], pos[0] ) * (180 / M_PI);
			da = angle - a;
		}

		for (int i = 0; i < panim->numpiecewisekeys; i++)
		{
			VectorYawRotate( panim->piecewisemove[i].pos, da, panim->piecewisemove[i].pos );
			VectorYawRotate( panim->piecewisemove[i].vector, da, panim->piecewisemove[i].vector );
		}
	}
	else
	{
		// look for movement in root bone
		Vector pos = panim->sanim[(panim->numframes - 1)][g_rootIndex].pos - panim->sanim[0][g_rootIndex].pos;
		if (pos[0] != 0 || pos[1] != 0)
		{
			float a = atan2( pos[1], pos[0] ) * (180 / M_PI);
			da = angle - a;
		}
	}

	/*
	if (da > -0.01 && da < 0.01)
		return;
	*/

	matrix3x4_t rootxform;
	matrix3x4_t src;
	matrix3x4_t dest;

	AngleMatrix( QAngle( 0, da, 0), rootxform );

	for (int j = 0; j < panim->numframes; j++)
	{
		for (int k = 0; k < g_numbones; k++)
		{
			if (g_bonetable[k].parent == -1)
			{
				QuaternionMatrix( panim->sanim[j][k].qrot, panim->sanim[j][k].pos, src );
				ConcatTransforms( rootxform, src, dest );
				MatrixQuaternion( dest, panim->sanim[j][k].qrot, panim->sanim[j][k].pos );
			}
		}
	}

	// FIXME: not finished
}

//-----------------------------------------------------------------------------
// Purpose: convert pBoneToWorld back into rot/pos data
//-----------------------------------------------------------------------------

void solveBone( 
	s_animation_t *panim,
	int iFrame,
	int	iBone,
	matrix3x4_t* pBoneToWorld
	)
{
	int iParent = g_bonetable[iBone].parent;

	if (iParent == -1)
	{
		MatrixQuaternion( pBoneToWorld[iBone], panim->sanim[iFrame][iBone].qrot, panim->sanim[iFrame][iBone].pos );
		return;
	}

	matrix3x4_t worldToBone;
	MatrixInvert( pBoneToWorld[iParent], worldToBone );

	matrix3x4_t local;
	ConcatTransforms( worldToBone, pBoneToWorld[iBone], local );

	iFrame = iFrame % panim->numframes;

	MatrixQuaternion( local, panim->sanim[iFrame][iBone].qrot, panim->sanim[iFrame][iBone].pos );
}


//-----------------------------------------------------------------------------
// Purpose: calc the influence of a ik rule for a specific point in the animation cycle
//-----------------------------------------------------------------------------

float IKRuleWeight( s_ikrule_t *pRule, float flCycle )
{
	if (pRule->end > 1.0f && flCycle < pRule->start)
	{
		flCycle = flCycle + 1.0f;
	}

	float value = 0.0f;
	if (flCycle < pRule->start)
	{
		return 0.0f;
	}
	else if (flCycle < pRule->peak )
	{
		value = (flCycle - pRule->start) / (pRule->peak - pRule->start);
	}
	else if (flCycle < pRule->tail )
	{
		return 1.0f;
	}
	else if (flCycle < pRule->end )
	{
		value = 1.0f - ((flCycle - pRule->tail) / (pRule->end - pRule->tail));
	}
	return 3.0f * value * value - 2.0f * value * value * value;
}


//-----------------------------------------------------------------------------
// Purpose: Lock the ik target to a specific location in order to clean up bad animations (shouldn't be needed).
//-----------------------------------------------------------------------------
void fixupIKErrors( s_animation_t *panim, s_ikrule_t *pRule )
{
	int k;

	if (pRule->start == 0 && pRule->peak == 0 && pRule->tail == 0 && pRule->end == 0)
	{
		pRule->tail = panim->numframes - 1;
		pRule->end = panim->numframes - 1;
	}

	// check for wrapping
	if (pRule->peak < pRule->start)
	{
		pRule->peak += panim->numframes - 1;
	}
	if (pRule->tail < pRule->peak)
	{
		pRule->tail += panim->numframes - 1;
	}
	if (pRule->end < pRule->tail)
	{
		pRule->end += panim->numframes - 1;
	}

	if (pRule->contact == -1)
	{
		pRule->contact = pRule->peak;
	}

	if (panim->numframes <= 1)
		return;

	pRule->errorData.numerror = pRule->end - pRule->start + 1;
	
	switch( pRule->type )
	{
	case IK_SELF:
#if 0
		// this code has never been run.....
		{
			matrix3x4_t boneToWorld[MAXSTUDIOBONES];
			matrix3x4_t worldToBone;
			matrix3x4_t local;
			Vector targetPos;
			Quaternion targetQuat;

			pRule->bone = findGlobalBone( pRule->bonename );
			if (pRule->bone == -1)
			{
				MdlError("unknown bone '%s' in ikrule\n", pRule->bonename );
			}

			matrix3x4_t srcBoneToWorld[MAXSTUDIOSRCBONES];
			BuildRawTransforms( panim->source, pRule->contact + panim->startframe - panim->source->startframe, srcBoneToWorld );
			TranslateAnimations( panim->source, srcBoneToWorld, boneToWorld );

			MatrixInvert( boneToWorld[pRule->bone], worldToBone );
			ConcatTransforms( worldToBone, boneToWorld[g_ikchain[pRule->chain].link[2].bone], local );
			MatrixAngles( local, targetQuat, targetPos );

			for (k = 0; k < pRule->errorData.numerror; k++)
			{
				BuildRawTransforms( panim->source, k + pRule->start + panim->startframe - panim->source->startframe, srcBoneToWorld );
				TranslateAnimations( panim->source, srcBoneToWorld, boneToWorld );

				float cycle = (panim->numframes <= 1) ? 0 : (k + pRule->start) / (panim->numframes - 1);
				float s = IKRuleWeight( pRule, cycle );

				Quaternion curQuat;
				Vector curPos;

				// convert into rule bone space
				MatrixInvert( boneToWorld[pRule->bone], worldToBone );
				ConcatTransforms( worldToBone, boneToWorld[g_ikchain[pRule->chain].link[2].bone], local );
				MatrixAngles( local, curQuat, curPos );

				// find blended rule bone relative position
				Vector rulePos = curPos * s + targetPos * (1.0 - s);
				Quaternion ruleQuat;
				QuaternionSlerp( curQuat, targetQuat, s, ruleQuat );
				QuaternionMatrix( ruleQuat, rulePos, local );

				Vector worldPos;
				VectorTransform( rulePos, boneToWorld[pRule->bone], worldPos );

				// printf("%d (%d) : %.1f %.1f %1.f\n", k + pRule->start, pRule->peak, pos.x, pos.y, pos.z );
				Studio_SolveIK(
					g_ikchain[pRule->chain].link[0].bone,
					g_ikchain[pRule->chain].link[1].bone,
					g_ikchain[pRule->chain].link[2].bone,
					worldPos,
					boneToWorld );

				// slam final matrix
				// FIXME: this isn't taking into account the IK may have failed
				ConcatTransforms( boneToWorld[pRule->bone], local, boneToWorld[g_ikchain[pRule->chain].link[2].bone] );

				solveBone( panim, k + pRule->start, g_ikchain[pRule->chain].link[0].bone, boneToWorld );  
				solveBone( panim, k + pRule->start, g_ikchain[pRule->chain].link[1].bone, boneToWorld );  
				solveBone( panim, k + pRule->start, g_ikchain[pRule->chain].link[2].bone, boneToWorld );  
			}
		}
#endif
		break;
	case IK_WORLD:
	case IK_GROUND:
		{
			matrix3x4a_t boneToWorld[MAXSTUDIOBONES];

			int bone = g_ikchain[pRule->chain].link[2].bone;
			CalcBoneTransforms( panim, pRule->contact, boneToWorld );
			// FIXME: add in motion

			Vector footfall;
			MatrixGetColumn( boneToWorld[bone], 3, footfall );

			//printf("%d %d %d %d (%d)\n", pRule->start, pRule->peak, pRule->tail, pRule->end, pRule->errorData.numerror );
			for (k = 0; k < pRule->errorData.numerror; k++)
			{
				CalcBoneTransforms( panim, k + pRule->start, boneToWorld );

				float cycle = (panim->numframes <= 1) ? 0 : (float)(k + pRule->start) / (panim->numframes - 1);
				float s = IKRuleWeight( pRule, cycle );
				s = 1.0; // FIXME - the weight rule is wrong

				Vector orig;
				MatrixPosition( boneToWorld[g_ikchain[pRule->chain].link[2].bone], orig );

				Vector pos = (footfall + calcMovement( panim, k + pRule->start, pRule->contact )) * s + orig * (1.0 - s);

				//printf("%d (%.1f:%.1f) : %.1f %.1f %1.f\n", k + pRule->start, cycle, s, pos.x, pos.y, pos.z );

				Studio_SolveIK(
					g_ikchain[pRule->chain].link[0].bone,
					g_ikchain[pRule->chain].link[1].bone,
					g_ikchain[pRule->chain].link[2].bone,
					pos,
					boneToWorld );

				solveBone( panim, k + pRule->start, g_ikchain[pRule->chain].link[0].bone, boneToWorld );  
				solveBone( panim, k + pRule->start, g_ikchain[pRule->chain].link[1].bone, boneToWorld );  
				solveBone( panim, k + pRule->start, g_ikchain[pRule->chain].link[2].bone, boneToWorld );  
			}
		}	
	}
	forceAnimationLoop( panim ); // !!!
}


//-----------------------------------------------------------------------------
// Purpose: map the vertex animations to their equivalent vertex in the base animations
//-----------------------------------------------------------------------------
static void ComputeSideAndScale( const s_flexkey_t &flexKey, s_vertanim_t *pVAnim, float *pSide, float *pScale )
{
	*pScale = 1.0f;
	*pSide = 0.0f;

	if ( flexKey.split > 0.0f )
	{
		if ( pVAnim->pos.x > flexKey.split ) 
		{
			*pScale = 0.0f;
		}
		else if ( pVAnim->pos.x < -flexKey.split ) 
		{
			*pScale = 1.0f;
		}
		else 
		{
			float t = ( flexKey.split - pVAnim->pos.x ) / (2.0 * flexKey.split);
			*pScale = 3.0f * t * t - 2.0f * t * t * t;
			// printf( "%.1f : %.2f\n", pSrcAnim->pos.x, *pScale );
		}
	}
	else if ( flexKey.split < 0.0f )
	{
		if ( pVAnim->pos.x < flexKey.split) 
		{
			*pScale = 0.0f;
		}
		else if ( pVAnim->pos.x > -flexKey.split) 
		{
			*pScale = 1.0f;
		}
		else 
		{
			float t = ( flexKey.split - pVAnim->pos.x ) / ( 2.0f * flexKey.split );
			*pScale = 3.0f * t * t - 2.0f * t * t * t;
			// printf( "%.1f : %.2f\n", pSrcAnim->pos.x, *pScale );
		}
	}

	if ( flexKey.flexpair != 0)
	{
		// paired flexes are full scale but variable side to side
		*pSide = 1.0 - *pScale;
		*pScale = 1.0;
	}
	else
	{
		// unpaired flexes are variable scale, one sided
		*pSide = 0;
	}
}


//-----------------------------------------------------------------------------
// Purpose: map the vertex animations to their equivalent vertex in the base animations
//-----------------------------------------------------------------------------
static void ComputeVertexAnimationSpeed( s_flexkey_t& flexKey )
{
	// calc max total scale for deltas
	float flScale = 0.0f;
	for ( int m = 0; m < flexKey.numvanims; m++ )
	{
		float s =flexKey.vanim[m].pos.Length();

		if ( s > flScale )
		{
			flScale = s;
		}
	}
	if ( flScale == 0.0f )
	{
		flScale = 0.01f;
	}

	// set 
	for ( int m = 0; m < flexKey.numvanims; m++ )
	{
		if ( flexKey.decay == 0.0f )
		{
			flexKey.vanim[m].speed = 1.0f;
		}
		else
		{
			flexKey.vanim[m].speed = clamp( flexKey.vanim[m].pos.Length() / (flScale * flexKey.decay), 0.0f, 1.0f );
		}
	}
}



//-----------------------------------------------------------------------------
// Purpose: map the vertex animations to their equivalent vertex in the base animations
//-----------------------------------------------------------------------------
static void BuildVAnimFlags( s_source_t *pVSource, s_sourceanim_t *pVSourceAnim, int nCurrentFlexKey )
{
	pVSourceAnim->vanim_flag = (int *)calloc( pVSource->numvertices, sizeof( int ));
	for ( int n = nCurrentFlexKey; n < g_numflexkeys; n++ )
	{
		// make sure it's the current flex file and that it's not frame 0 (happens with eyeball stuff).
		if ( g_flexkey[n].source != pVSource )
			continue;

		if ( Q_stricmp( g_flexkey[n].animationname, pVSourceAnim->animationname ) )
			continue;

		const s_sourceanim_t *pAnim = FindSourceAnim( g_flexkey[n].source, g_flexkey[n].animationname );
		if ( !pAnim )
			continue;

		if ( pAnim->newStyleVertexAnimations != pVSourceAnim->newStyleVertexAnimations )
			continue;

		if ( !pAnim->newStyleVertexAnimations && g_flexkey[n].frame == 0 )
			continue;

		int k = g_flexkey[n].frame;
		for ( int m = 0; m < pVSourceAnim->numvanims[k]; m++ )
		{
			pVSourceAnim->vanim_flag[ pVSourceAnim->vanim[k][m].vertex ] = 1;
		}
	}
}

#define MAX_VANIM_DIST 0.3873f
#define MAX_VANIM_DIST_SQR ( MAX_VANIM_DIST * MAX_VANIM_DIST )

//-----------------------------------------------------------------------------
// Purpose: Build an array indexed by model vertex which indicates which vanim vertex corresponds best to it
//-----------------------------------------------------------------------------
static void BuildModelToVAnimMap( s_source_t *pVSource, s_sourceanim_t *pVSourceAnim, s_loddata_t *pmLodSource, bool bNewVertexAnimations, int *pModelToVAnim )
{
	static float imapdist[MAXSTUDIOSRCVERTS];	// distance from src vert to vanim vert
	static float imapdot[MAXSTUDIOSRCVERTS];	// dot product of src norm to vanim normal

	for ( int j = 0; j < pmLodSource->numvertices; j++ )
	{
		imapdist[j] = 1E30;
		imapdot[j] = -1.0;
		pModelToVAnim[j] = -1;
	}

	// Build a sphere tree to accelerate this search process:
	CUtlSphereTree sphereTree;
	int nMinLod = MIN( g_minLod, g_ScriptLODs.Count() - 1 );
	for ( int k = 0; k < pmLodSource->numvertices; k++ )
	{
		// go ahead and skip vertices that are just going to be stripped later
		// TODO: take this out when the lod clamping stuff gets moved into the LOD code instead of being a post process
		s_lodvertexinfo_t &vertex = pmLodSource->vertex[k];
		if ( nMinLod && !( vertex.lodFlag & (0xFFFFFF << nMinLod) ) )
			continue;

		Sphere_t sphere( vertex.position.x, vertex.position.y, vertex.position.z, 0 );
		sphereTree.Insert( (void *)k, &sphere );
	}

	int nError = 0, nTests = 0, nBumps = 0;
	float flErrorDist = 0.0f;
	CUtlVector<void *> candidates;
	float searchRadius = MAX_VANIM_DIST;

	// TODO: this would be faster if we inserted the pVSource into the spheretree instead (we could avoid 'scatter' writes to imapdist[] and imapdot[] in the inner loop)
	for ( int j = 0; j < pVSource->numvertices; j++ )
	{
		const Vector& vecModelPos    = bNewVertexAnimations ? pVSource->m_GlobalVertices[j].position : pVSourceAnim->vanim[0][j].pos;
		const Vector& vecModelNormal = bNewVertexAnimations ? pVSource->m_GlobalVertices[j].normal   : pVSourceAnim->vanim[0][j].normal;

		// Search for verts within a small radius (shrink the radius over time, to converge on a reasonable minimum search radius)
		Sphere_t searchSphere( vecModelPos.x, vecModelPos.y, vecModelPos.z, searchRadius );
		sphereTree.IntersectWithSphere( searchSphere, true, candidates, 0, NULL );
		while( !candidates.Count() && ( searchRadius < MAX_VANIM_DIST ) )
		{
			searchRadius = MIN( MAX_VANIM_DIST, searchRadius*2 );
			searchSphere.w = searchRadius;
			sphereTree.IntersectWithSphere( searchSphere, true, candidates, 0, NULL );
			nBumps++;
		}
		searchRadius = MAX( 0.001f*MAX_VANIM_DIST, searchRadius*0.95f );

		float flMinDist = 1E30;
		for ( int i = 0; i < candidates.Count(); i++ )
		{
			nTests++;
			int index = (int)candidates[i];

			s_lodvertexinfo_t &vertex = pmLodSource->vertex[index];

			// TODO: Length() gives inconsistent results in release build
			Vector tmp;
			VectorSubtract( vertex.position, vecModelPos, tmp );
			float flDist = tmp.LengthSqr();
			float flDot  = DotProduct( vertex.normal, vecModelNormal );

			if ( flDist < flMinDist )
				flMinDist = flDist;

			// Smallest distance wins. In case of a distance tie, biggest dot wins. If both tie, lowest index wins.
			if ( flDist < imapdist[index] || ( flDist == imapdist[index] && flDot > imapdot[index] ) )
			{
				imapdist[index] = flDist;
				imapdot[index] = flDot;
				pModelToVAnim[index] = j;
			}
		}

		if ( flMinDist > 0.01 )
		{
			nError++;
			flErrorDist += MIN( sqrtf( flMinDist ), MAX_VANIM_DIST ) ;
		}
	}

	if (nError)
	{
		MdlWarning("unmatched vertex anims %d (%.2f)\n", nError, flErrorDist / nError );
	}
}


//-----------------------------------------------------------------------------
// Purpose: Build an array indexed by model vertex which indicates which vanim vertex corresponds best to it
//-----------------------------------------------------------------------------
static void BuildVAnimMap( s_source_t *pVSource, s_sourceanim_t *pVSourceAnim, s_loddata_t *pmLodSource, const int *pModelToVAnim )
{
	// indexed by vertex anim vertex index
	static int *mapp[MAXSTUDIOVERTS*4];

	// count number of times each vanim vert connectes to a model vert
	int n = 0;
	pVSourceAnim->vanim_mapcount = (int *)calloc( pVSource->numvertices, sizeof( int ) );
	for ( int j = 0; j < pmLodSource->numvertices; j++ )
	{
		if ( pModelToVAnim[j] != -1 )
		{
			pVSourceAnim->vanim_mapcount[ pModelToVAnim[j] ]++;
			n++;
		}
	}

	pVSourceAnim->vanim_map = (int **)calloc( pVSource->numvertices, sizeof( int * ));
	int *vmap = (int *)calloc( n, sizeof( int ) );

	// build mapping arrays
	for ( int j = 0; j < pVSource->numvertices; j++ )
	{
		if ( pVSourceAnim->vanim_mapcount[j] )
		{
			pVSourceAnim->vanim_map[j] = vmap;
			mapp[j] = vmap;
			vmap += pVSourceAnim->vanim_mapcount[j];
		}
		else if ( pVSourceAnim->vanim_flag[j] )
		{
			// printf("%d animates but no matching vertex\n", j );
		}
	}

	for ( int j = 0; j < pmLodSource->numvertices; j++ )
	{
		if (pModelToVAnim[j] != -1)
		{
			*(mapp[ pModelToVAnim[j] ]++) = j;
		}
	}
}


//-----------------------------------------------------------------------------
// Computes the number of unique desination vanims, allocates space for it
//-----------------------------------------------------------------------------
static void AllocateDestVAnim( s_flexkey_t &flexKey, s_sourceanim_t *pVSourceAnim )
{
	int nVAnimCount = pVSourceAnim->numvanims[ flexKey.frame ];
	s_vertanim_t *pVAnim = pVSourceAnim->vanim[ flexKey.frame ];

	// frame 0 is special.  Always assume zero vertex animations
	if ( !pVSourceAnim->newStyleVertexAnimations && flexKey.frame == 0 )
	{
		nVAnimCount = 0;
	}

	// count total possible remapped animations
	int nNumDestVAnims = 0;
	for ( int m = 0; m < nVAnimCount; m++)
	{
		nNumDestVAnims += pVSourceAnim->vanim_mapcount[ pVAnim[m].vertex ];
	}

	// allocate room to all possible resulting deltas
	s_vertanim_t *pDestAnim = (s_vertanim_t *)calloc( nNumDestVAnims, sizeof( s_vertanim_t ) );
	flexKey.vanim = pDestAnim;
	flexKey.vanimtype = STUDIO_VERT_ANIM_NORMAL;	// default
}


//-----------------------------------------------------------------------------
// Purpose: map the vertex animations to their equivalent vertex in the base animations
//-----------------------------------------------------------------------------
void RemapVertexAnimations(void)
{
	int i, j, k;
	int n, m;
	s_source_t	*pvsource;					// vertex animation source
	const char *pAnimationName;
	s_sourceanim_t *pSourceAnim;
	s_loddata_t	*pmLodSource;				// original model source
	Vector		tmp;

	// index by vertex in targets root LOD
	static int			model_to_vanim_vert_imap[MAXSTUDIOSRCVERTS];		// model vert to vanim vert mapping

	// for all the sources of flexes, find a mapping of vertex animations to base model.
	// There can be multiple "vertices" in the base model for each animated vertex since vertices 
	// are duplicated along material boundaries.
	for ( i = 0; i < g_numflexkeys; i++ )
	{
		s_source_t *pVSource = g_flexkey[i].source;
		s_sourceanim_t *pVSourceAnim = FindSourceAnim( pVSource, g_flexkey[i].animationname );

		// We only do old-style vertex animations
		if ( pVSourceAnim->newStyleVertexAnimations )
			continue;

		// skip if it's already been done or if has doesn't have any animations
		if ( pVSourceAnim->vanim_flag )
			continue;

		// flag all the vertices that animate (builds the vanim_flag field of the source anim)
		BuildVAnimFlags( pVSource, pVSourceAnim, i );

		s_loddata_t *pLodData = g_model[ g_flexkey[i].imodel ]->m_pLodData;

		// Map vertex indices specified in the model to ones specified in the vanim data
		BuildModelToVAnimMap( pVSource, pVSourceAnim, pLodData, false, model_to_vanim_vert_imap );

		// Build the vanim_mapcount, vanim_map fields of the source anim
		BuildVAnimMap( pVSource, pVSourceAnim, pLodData, model_to_vanim_vert_imap );
	}

#if 0
	s_vertanim_t *defaultanims = NULL;

	if (g_defaultflexkey)
	{
		defaultanims = g_defaultflexkey->source->vanim[g_defaultflexkey->frame];
	}
	else
	{
		defaultanims = g_flexkey[0].source->vanim[0];
	}
#endif

	// reset model to be default animations
	if ( g_defaultflexkey )
	{
		pvsource = g_defaultflexkey->source;
		pAnimationName = g_defaultflexkey->animationname;
		pSourceAnim = FindSourceAnim( pvsource, pAnimationName );
		pmLodSource = g_model[g_defaultflexkey->imodel]->m_pLodData;

		int numsrcanims = pSourceAnim->numvanims[g_defaultflexkey->frame];
		s_vertanim_t *psrcanim = pSourceAnim->vanim[g_defaultflexkey->frame];

		for (m = 0; m < numsrcanims; m++)
		{
			if ( pSourceAnim->vanim_mapcount[psrcanim->vertex]) // bah, only do it for ones that found a match!
			{
				for (n = 0; n < pSourceAnim->vanim_mapcount[psrcanim->vertex]; n++)
				{
					// copy "default" pos to original model
					k = pSourceAnim->vanim_map[psrcanim->vertex][n];
					VectorCopy( psrcanim->pos, pmLodSource->vertex[k].position );
					VectorCopy( psrcanim->normal, pmLodSource->vertex[k].normal );

					// copy "default" pos to frame 0 of vertex animation source
					// FIXME: this needs to copy to all sources of vertex animation.
					// FIXME: the "default" pose needs to be in each vertex animation source since it's likely that the vertices won't be numbered the same in each file.
					VectorCopy( psrcanim->pos, pSourceAnim->vanim[0][psrcanim->vertex].pos );
					VectorCopy( psrcanim->normal, pSourceAnim->vanim[0][psrcanim->vertex].normal );
				}
			}
			psrcanim++;
		}
	}

	static bool			doesMove[MAXSTUDIOSRCVERTS];
	int					numMoved;

	memset( doesMove, 0, MAXSTUDIOSRCVERTS * sizeof( bool ) );
	numMoved = 0;

	for (i = 0; i < g_numflexkeys; i++)
	{
		pvsource = g_flexkey[i].source;
		pAnimationName = g_flexkey[i].animationname;
		pSourceAnim = FindSourceAnim( pvsource, pAnimationName );
		if ( pSourceAnim->newStyleVertexAnimations )
			continue;

		pmLodSource = g_model[g_flexkey[i].imodel]->m_pLodData;

		// Allocate g_flexkey[i].vanim
		AllocateDestVAnim( g_flexkey[i], pSourceAnim );

		s_vertanim_t *psrcanim = pSourceAnim->vanim[g_flexkey[i].frame];
		s_vertanim_t *pdestanim = g_flexkey[i].vanim;

		// frame 0 is special.  Always assume zero vertex animations
		int numsrcanims = ( g_flexkey[i].frame != 0 ) ? pSourceAnim->numvanims[g_flexkey[i].frame] : 0;

		for (m = 0; m < numsrcanims; m++, psrcanim++)
		{
			Vector delta, ndelta;
			float flSide, flScale;
			ComputeSideAndScale( g_flexkey[i], psrcanim, &flSide, &flScale );

			// bah, only do it for ones that found a match!
			if ( flScale <= 0.0f || !pSourceAnim->vanim_mapcount[psrcanim->vertex] )
				continue;

			j = pSourceAnim->vanim_map[psrcanim->vertex][0];

			//VectorSubtract( psrcanim->pos, pSourceAnim->vanim[0][psrcanim->vertex].pos, tmp );
			//VectorTransform( tmp, pmsource->bonefixup[k].im, delta );
			VectorSubtract( psrcanim->pos, pSourceAnim->vanim[0][psrcanim->vertex].pos, delta );

			//VectorSubtract( psrcanim->normal, pSourceAnim->vanim[0][psrcanim->vertex].normal, tmp );
			//VectorTransform( tmp, pmsource->bonefixup[k].im, ndelta );
			VectorSubtract( psrcanim->normal, pSourceAnim->vanim[0][psrcanim->vertex].normal, ndelta );

			// if the changes are too small, skip 'em
			// FIXME: the clamp needs to be paired with the other matching positions.
			// currently this is set to the float16 min value.  Sucky.
			if (DotProduct( delta, delta ) <= (0.001f*0.001f) /* 0.0001 */ && DotProduct( ndelta, ndelta ) <= 0.001)
			{
				// printf("%4d  %6.4f %6.4f %6.4f\n", pdestanim->vertex, delta.x, delta.y, delta.z );
				continue;
			}

			for (n = 0; n < pSourceAnim->vanim_mapcount[psrcanim->vertex]; n++)
			{
				pdestanim->vertex = pSourceAnim->vanim_map[psrcanim->vertex][n];
				VectorScale( delta, flScale, pdestanim->pos );
				VectorScale( ndelta, flScale, pdestanim->normal );
				pdestanim->side = flSide;

				// count all the unique verts that actually move
				if (!doesMove[pdestanim->vertex])
				{
					doesMove[pdestanim->vertex] = true;
					numMoved++;
				}

				/*
				printf("%4d  %6.2f %6.2f %6.2f : %4d  %5.2f %5.2f %5.2f\n", 
				pdestanim->vertex, 
				// pmsource->vertex[pdestanim->vertex][0], pmsource->vertex[pdestanim->vertex][1], pmsource->vertex[pdestanim->vertex][2],
				modelpos[pdestanim->vertex][0], modelpos[pdestanim->vertex][1], modelpos[pdestanim->vertex][2],
				psrcanim->vertex,					
				pdestanim->pos[0], pdestanim->pos[1], pdestanim->pos[2] );
				*/
				g_flexkey[i].numvanims++;
				pdestanim++;
			}
		}

		ComputeVertexAnimationSpeed( g_flexkey[i] );
	}

	if (numMoved > MAXSTUDIOFLEXVERTS)
	{
		MdlError( "Too many flexed verts %d (%d)\n", numMoved, MAXSTUDIOFLEXVERTS );
	}
	else if (numMoved > 0 && !g_quiet)
	{
		printf("Max flex verts %d\n", numMoved );
	}
}


//-----------------------------------------------------------------------------
// Purpose: map the vertex animations to their equivalent vertex in the base animations
//-----------------------------------------------------------------------------
static int FlexKeysSortFunc( const void *pv1, const void *pv2 ) 
{
	const s_flexkey_t *pKey1 = (const s_flexkey_t*)pv1;
	const s_flexkey_t *pKey2 = (const s_flexkey_t*)pv2;

	if ( pKey1->source != pKey2->source )
		return (size_t)pKey1->source - (size_t)pKey2->source;
	return Q_stricmp( pKey1->animationname, pKey2->animationname );
}

static int SortFlexKeys( s_flexkey_t **ppSortedFlexKeys )
{
	int nSortedFlexKeyCount = 0;
	for ( int i = 0; i < g_numflexkeys; i++ )
	{
		s_source_t *pVSource = g_flexkey[i].source;
		s_sourceanim_t *pVSourceAnim = FindSourceAnim( pVSource, g_flexkey[i].animationname );

		// We only do new-style vertex animations
		if ( !pVSourceAnim->newStyleVertexAnimations )
			continue;

		ppSortedFlexKeys[nSortedFlexKeyCount++] = &g_flexkey[i];
	}

	if ( nSortedFlexKeyCount > 0 )
	{
		qsort( ppSortedFlexKeys, nSortedFlexKeyCount, sizeof(s_flexkey_t*), FlexKeysSortFunc );
	}

	return nSortedFlexKeyCount;
}

static void RemapVertexAnimationsNewVersion(void)
{
	// index by vertex in targets root LOD
	static int model_to_vanim_vert_imap[MAXSTUDIOSRCVERTS];

	// Sort flexkeys by source
	s_flexkey_t **ppSortedFlexKeys = (s_flexkey_t**)_alloca( g_numflexkeys * sizeof(s_flexkey_t*) );
	int nSortedFlexKeyCount = SortFlexKeys( ppSortedFlexKeys );
	if ( nSortedFlexKeyCount == 0 )
		return;

	// for all the sources of flexes, find a mapping of vertex animations to base model.
	// There can be multiple "vertices" in the base model for each animated vertex since vertices 
	// are duplicated along material boundaries.
	s_source_t *pVLastSource = NULL;
	for ( int i = 0; i < nSortedFlexKeyCount; i++ )
	{
		s_flexkey_t *pFlexKey = ppSortedFlexKeys[i];
		s_source_t *pVSource = pFlexKey->source;
		s_sourceanim_t *pVSourceAnim = FindSourceAnim( pVSource, pFlexKey->animationname );
		s_loddata_t *pLodSource = g_model[ pFlexKey->imodel ]->m_pLodData;

		if ( pVSource != pVLastSource )
		{
			// Map vertex indices specified in the model to ones specified in the vanim data
			BuildModelToVAnimMap( pVSource, NULL, pLodSource, true, model_to_vanim_vert_imap );
			pVLastSource = pVSource;
		}

		// We only do new-style vertex animations
		Assert( pVSourceAnim->newStyleVertexAnimations );

		// skip if it's already been done or if has doesn't have any animations
		if ( pVSourceAnim->vanim_flag )
			continue;

		pVSourceAnim->vanim_flag = (int *)calloc( pVSource->numvertices, sizeof( int ));

		// flag all the vertices that animate (builds the vanim_flag field of the source anim)
		int j;
		for ( j = i+1; j < nSortedFlexKeyCount; ++j )
		{
			if ( ( ppSortedFlexKeys[j]->source != pVSource ) || 
				  Q_stricmp( ppSortedFlexKeys[j]->animationname, pFlexKey->animationname ) )
				break;
		}

		for ( ; i < j; ++i )
		{
			int k = ppSortedFlexKeys[i]->frame;
			for ( int m = 0; m < pVSourceAnim->numvanims[k]; m++ )
			{
				pVSourceAnim->vanim_flag[ pVSourceAnim->vanim[k][m].vertex ] = 1;
			}
		}
		--i;

		// Build the vanim_mapcount, vanim_map fields of the source anim
		BuildVAnimMap( pVSource, pVSourceAnim, pLodSource, model_to_vanim_vert_imap );
	}

	int nNumMoved = 0;
	static bool	pDoesMove[MAXSTUDIOSRCVERTS];
	memset( pDoesMove, 0, MAXSTUDIOSRCVERTS * sizeof( bool ) );

	for ( int i = 0; i < g_numflexkeys; i++ )
	{
		s_source_t *pVSource = g_flexkey[i].source;
		s_sourceanim_t *pVSourceAnim = FindSourceAnim( pVSource, g_flexkey[i].animationname );
		if ( !pVSourceAnim->newStyleVertexAnimations )
			continue;

		// Allocate g_flexkey[i].vanim
		AllocateDestVAnim( g_flexkey[i], pVSourceAnim );

		int nNumSrcVAnims = pVSourceAnim->numvanims[ g_flexkey[i].frame ];
		s_vertanim_t *pSrcVAnim = pVSourceAnim->vanim[ g_flexkey[i].frame ];
		s_vertanim_t *pDestVAnim = g_flexkey[i].vanim;

		for ( int m = 0; m < nNumSrcVAnims; m++, pSrcVAnim++ )
		{
			// bah, only do it for ones that found a match!
			if ( !pVSourceAnim->vanim_mapcount[pSrcVAnim->vertex] )
				continue;

			// if the changes are too small, skip 'em
			// FIXME: the clamp needs to be paired with the other matching positions.
			// currently this is set to the float16 min value.  Sucky.
			if ( DotProduct( pSrcVAnim->pos, pSrcVAnim->pos ) <= (0.001f*0.001f) /* 0.0001 */ && DotProduct( pSrcVAnim->normal, pSrcVAnim->normal ) <= 0.001f && pSrcVAnim->wrinkle <= 0.001f )
			{
			//	printf("%4d  %6.4f %6.4f %6.4f\n", pDestAnim->vertex, delta.x, delta.y, delta.z );
				continue;
			}

			for ( int n = 0; n < pVSourceAnim->vanim_mapcount[pSrcVAnim->vertex]; n++ )
			{
				memcpy( pDestVAnim, pSrcVAnim, sizeof(s_vertanim_t) );
				pDestVAnim->vertex = pVSourceAnim->vanim_map[pSrcVAnim->vertex][n];

				if ( pDestVAnim->wrinkle != 0.0f )
				{
					g_flexkey[i].vanimtype = STUDIO_VERT_ANIM_WRINKLE;
				}

				// count all the unique verts that actually move
				if ( !pDoesMove[pDestVAnim->vertex] )
				{
					pDoesMove[pDestVAnim->vertex] = true;
					nNumMoved++;
				}

				g_flexkey[i].numvanims++;
				pDestVAnim++;
			}
		}
	}

	if ( nNumMoved > MAXSTUDIOFLEXVERTS )
	{
		MdlError( "Too many flexed verts %d (%d)\n", nNumMoved, MAXSTUDIOFLEXVERTS );
	}
	else if ( nNumMoved > 0 && !g_quiet )
	{
		printf("Max flex verts %d\n", nNumMoved );
	}
}


// Finds the bone index for a particular source
extern int FindLocalBoneNamed( const s_source_t *pSource, const char *pName );

//-----------------------------------------------------------------------------
// Purpose: finds the bone index in the global bone table
//-----------------------------------------------------------------------------

int findGlobalBone( const char *name )
{
	name = RenameBone( name );
	for ( int k = 0; k < g_numbones; k++ )
	{
		if ( !Q_stricmp( g_bonetable[k].name, name ) )
			return k;
	}
	
	return -1;
}


bool IsGlobalBoneXSI( const char *name, const char *bonename )
{
	name = RenameBone( name );

	int len = strlen( name );

	int len2 = strlen( bonename );
	if ( len2 == len && strchr( bonename, '.' ) == NULL && stricmp( bonename, name ) == 0 )
			return true;

	if (len2 > len)
	{

		if (bonename[len2-len-1] == '.')
		{
			if (stricmp( &bonename[len2-len], name ) == 0)
			{
				return true;
			}
		}
	}
	
	return false;
}



int findGlobalBoneXSI( const char *name )
{
	int k;

	name = RenameBone( name );

	for (k = 0; k < g_numbones; k++)
	{
		if (IsGlobalBoneXSI( name, g_bonetable[k].name ))
		{
			return k;
		}
	}
	
	return -1;
}

//-----------------------------------------------------------------------------
// Purpose: Acculumate quaternions and try to find the swept area of rotation 
//			so that a "midpoint" of the rotation area can be found
//-----------------------------------------------------------------------------

void findAnimQuaternionAlignment( int k, int i, Quaternion &qBase, Quaternion &qMin, Quaternion &qMax )
{

	MdlWarning("Trying to use deprecated findAnimQuaternionAlignment \n");

	
	int j;

	AngleQuaternion( g_panimation[i]->sanim[0][k].rot, qBase );
	qMin = qBase;
	float dMin = 1.0;
	qMax = qBase;
	float dMax = 1.0;
	
	for (j = 1; j < g_panimation[i]->numframes; j++)
	{
		Quaternion q;

		AngleQuaternion( g_panimation[i]->sanim[j][k].rot, q );
		QuaternionAlign( qBase, q, q );

		float d0 = QuaternionDotProduct( q, qBase );
		float d1 = QuaternionDotProduct( q, qMin );
		float d2 = QuaternionDotProduct( q, qMax );

		
		//if (i != 0) printf("%f %f %f : %f\n", d0, d1, d2, QuaternionDotProduct( qMin, qMax ) );
		
		if (d1 >= d0)
		{
			if (d0 < dMin)
			{
				qMin = q;
				dMin = d0;
				if (dMax == 1.0)
				{
					QuaternionMA( qBase, -0.01, qMin, qMax );
					QuaternionAlign( qBase, qMax, qMax );
				}
			}
		}
		else if (d2 >= d0)
		{
			if (d0 < dMax)
			{
				qMax = q;
				dMax = d0;
			}
		}

		//if (i != 0) printf("%f ", QuaternionDotProduct( qMin, qMax ) );

		QuaternionSlerpNoAlign( qMin, qMax, 0.5, qBase );
		Assert( qBase.IsValid() );

		/*
		if (i != 0)
		{
			QAngle ang;
			QuaternionAngles( qMin, ang );
			printf("(%.1f %.1f %.1f) ", ang.x, ang.y, ang.z );
			QuaternionAngles( qMax, ang );
			printf("(%.1f %.1f %.1f) ", ang.x, ang.y, ang.z );
			QuaternionAngles( qBase, ang );
			printf("(%.1f %.1f %.1f)\n", ang.x, ang.y, ang.z );
		}
		*/

		dMin = QuaternionDotProduct( qBase, qMin );
		dMax = QuaternionDotProduct( qBase, qMax );
	}

	// printf("%s (%s): %.3f :%.3f\n", g_bonetable[k].name, g_panimation[i]->name, QuaternionDotProduct( qMin, qMax ), QuaternionDotProduct( qMin, qBase ) );
	/*
	if (i != 0) 
		exit(0);
	*/
}


//-----------------------------------------------------------------------------
// Purpose: For specific bones, try to find the total valid area of rotation so 
//			that their mid point of rotation can be used at run time to "pre-align"
//			the quaternions so that rotations > 180 degrees don't get blended the 
//			"short way round".
//-----------------------------------------------------------------------------

void limitBoneRotations( void )
{


	MdlWarning("Trying to use deprecated limitBoneRotations (will broke animations) \n");

	int i, j, k;

	for (i = 0; i < g_numlimitrotation; i++)
	{
		Quaternion qBase;

		k = findGlobalBone( g_limitrotation[i].name );
		if (k == -1)
		{
			MdlError("unknown bone \"%s\" in $limitrotation\n", g_limitrotation[i].name );
		}

		AngleQuaternion( g_bonetable[k].rot, qBase );

		if (g_limitrotation[i].numseq == 0)
		{
			for (j = 0; j < g_numani; j++)
			{
				if (!(g_panimation[j]->flags & STUDIO_DELTA) && g_panimation[j]->numframes > 3)
				{
					Quaternion qBase2, qMin2, qMax2;
					findAnimQuaternionAlignment( k, j, qBase2, qMin2, qMax2 );

					QuaternionAdd( qBase, qBase2, qBase );
				}
			}
			QuaternionNormalize( qBase );
		}
		else
		{
			for (j = 0; j < g_limitrotation[i].numseq; j++)
			{

			}
		}

		/*
		QAngle ang;
		QuaternionAngles( qBase, ang );
		printf("%s : (%.1f %.1f %.1f) \n", g_bonetable[k].name, ang.x, ang.y, ang.z );
		*/

		g_bonetable[k].qAlignment = qBase;
		g_bonetable[k].flags |= BONE_FIXED_ALIGNMENT;

		// QuaternionAngles( qBase, g_panimation[0]->sanim[0][k].rot );
	}
}





//-----------------------------------------------------------------------------
// Purpose: For specific bones, try to find the total valid area of rotation so 
//			that their mid point of rotation can be used at run time to "pre-align"
//			the quaternions so that rotations > 180 degrees don't get blended the 
//			"short way round".
//-----------------------------------------------------------------------------

void limitIKChainLength( void )
{
	int i, j, k;
	matrix3x4_t boneToWorld[MAXSTUDIOSRCBONES];	// bone transformation matrix

	for (k = 0; k < g_numikchains; k++)
	{
		bool needsFixup = false;
		bool hasKnees = false;

		Vector kneeDir = g_ikchain[k].link[0].kneeDir;
		if (kneeDir.Length() > 0.0)
		{
			hasKnees = true;
		}
		else
		{
			for (i = 0; i < g_numani; i++)
			{
				s_animation_t *panim = g_panimation[i];

				if (panim->flags & STUDIO_DELTA)
					continue;

				if (panim->flags & STUDIO_HIDDEN)
					continue;

				for (j = 0; j < panim->numframes; j++)
				{
					CalcBoneTransforms( panim, j, boneToWorld );

					Vector worldThigh;
					Vector worldKnee;
					Vector worldFoot;

					MatrixPosition( boneToWorld[ g_ikchain[k].link[0].bone ], worldThigh );
					MatrixPosition( boneToWorld[ g_ikchain[k].link[1].bone ], worldKnee );
					MatrixPosition( boneToWorld[ g_ikchain[k].link[2].bone ], worldFoot );

					float l1 = (worldKnee-worldThigh).Length();
					float l2 = (worldFoot-worldKnee).Length();
					float l3 = (worldFoot-worldThigh).Length();

					Vector ikHalf = (worldFoot+worldThigh) * 0.5;

					// FIXME: what to do when the knee completely straight?
					Vector ikKneeDir = worldKnee - ikHalf;
					VectorNormalize( ikKneeDir );
					// ikTargetKnee = ikKnee + ikKneeDir * l1;

					// leg too straight to figure out knee?
					if (l3 > (l1 + l2) * 0.999)
					{
						needsFixup = true;
					}
					else
					{
						// rotate knee into local space
						Vector tmp;
						VectorIRotate( ikKneeDir, boneToWorld[ g_ikchain[k].link[0].bone ], tmp );
						float bend = (((DotProduct( worldThigh - worldKnee, worldFoot - worldKnee ) ) / (l1 * l3)) + 1) / 2.0;
						kneeDir += tmp * bend;
						hasKnees = true;
					}
				}
			}
		}

		if (!needsFixup)
			continue;

		if (!hasKnees)
		{
			MdlWarning( "ik rules for %s but no clear knee direction\n", g_ikchain[k].name );
			continue;
		}

		VectorNormalize( kneeDir );
		g_ikchain[k].link[0].kneeDir = kneeDir;

		if (g_verbose)
		{
			printf("knee %s %f %f %f\n", g_ikchain[k].name, kneeDir.x, kneeDir.y, kneeDir.z );
		}

#if 0
		// don't bother for now, storing the knee direction should fix the runtime problems.
		for (i = 0; i < g_numani; i++)
		{
			s_animation_t *panim = g_panimation[i];

			if (panim->flags & STUDIO_DELTA)
				continue;

			for (j = 0; j < panim->numframes; j++)
			{
				CalcBoneTransforms( panim, j, boneToWorld );

				Vector worldFoot;
				MatrixPosition( boneToWorld[ g_ikchain[k].link[2].bone ], worldFoot );

				Vector targetKneeDir;
				VectorRotate( kneeDir, boneToWorld[ g_ikchain[k].link[0].bone ], targetKneeDir );

				// run it through the normal IK solver, this should move the foot positions to someplace legal
				Studio_SolveIK( g_ikchain[k].link[0].bone, g_ikchain[k].link[1].bone, g_ikchain[k].link[2].bone, worldFoot, targetKneeDir, boneToWorld );

				solveBone( panim, j, g_ikchain[k].link[0].bone, boneToWorld );  
				solveBone( panim, j, g_ikchain[k].link[1].bone, boneToWorld );  
				solveBone( panim, j, g_ikchain[k].link[2].bone, boneToWorld );  
			}
		}
#endif
	}
}


//-----------------------------------------------------------------------------
// Purpose: build "next node" table that links every transition "node" to 
//			every other transition "node", if possible
//-----------------------------------------------------------------------------
void MakeTransitions( )
{
	int i, j, k;
	bool iHit = g_bMultistageGraph;

	// add in direct node transitions
	for (i = 0; i < g_sequence.Count(); i++)
	{
		if (g_sequence[i].entrynode != g_sequence[i].exitnode)
		{
			g_xnode[g_sequence[i].entrynode-1][g_sequence[i].exitnode-1] = g_sequence[i].exitnode;
			if (g_sequence[i].nodeflags)
			{
				g_xnode[g_sequence[i].exitnode-1][g_sequence[i].entrynode-1] = g_sequence[i].entrynode;
			}
		}
	}

	// calculate multi-stage transitions 
	while (iHit)
	{
		iHit = false;
		for (i = 1; i <= g_numxnodes; i++)
		{
			for (j = 1; j <= g_numxnodes; j++)
			{
				// if I can't go there directly
				if (i != j && g_xnode[i-1][j-1] == 0)
				{
					for (k = 1; k <= g_numxnodes; k++)
					{
						// but I found someone who knows how that I can get to
						if (g_xnode[k-1][j-1] > 0 && g_xnode[i-1][k-1] > 0)
						{
							// then go to them
							g_xnode[i-1][j-1] = -g_xnode[i-1][k-1];
							iHit = true;
							break;
						}
					}
				}
			}
		}
		// reset previous pass so the links can be used in the next pass
		for (i = 1; i <= g_numxnodes; i++)
		{
			for (j = 1; j <= g_numxnodes; j++)
			{
				g_xnode[i-1][j-1] = abs( g_xnode[i-1][j-1] );
			}
		}
	}

	// add in allowed "skips"
	for (i = 0; i < g_numxnodeskips; i++)
	{
		g_xnode[g_xnodeskip[i][0]-1][g_xnodeskip[i][1]-1] = 0;
	}

	if (g_bDumpGraph)
	{
		for (j = 1; j <= g_numxnodes; j++)
		{
			printf("%2d : %s\n", j, g_xnodename[j] );
		}
		printf("    " );
		for (j = 1; j <= g_numxnodes; j++)
		{
			printf("%2d ", j );
		}
		printf("\n" );

		for (i = 1; i <= g_numxnodes; i++)
		{
			printf("%2d: ", i );
			for (j = 1; j <= g_numxnodes; j++)
			{
				printf("%2d ", g_xnode[i-1][j-1] );
			}
			printf("\n" );
		}
	}
}


int VectorCompareEpsilon(const Vector& v1, const Vector& v2, float epsilon)
{
	int		i;
	
	for (i=0 ; i<3 ; i++)
		if (fabs(v1[i] - v2[i]) > epsilon)
			return 0;
			
	return 1;
}

//int RadianEulerCompareEpsilon(const RadianEuler& v1, const RadianEuler& v2, float epsilon)
//{
//	int		i;
//	
//	for (i=0 ; i<3 ; i++)
//	{
//		// clamp to 2pi
//		float a1 = fmod(v1[i],(float) (2*M_PI));
//		float a2 = fmod(v2[i],(float) (2*M_PI));
//		float delta =  fabs(a1-a2);
//		
//		// use the smaller angle (359 == 1 degree off)
//		if ( delta > M_PI )
//		{
//			delta = 2*M_PI - delta;
//		}
//
//		if (delta > epsilon)
//			return 0;
//	}
//			
//	return 1;
//}

int QuaternionCompare(const Quaternion& q1, const Quaternion& q2)
{
	int		i;

	for (i = 0; i < 4; i++)
	{
		if (q1[i] != q2[i])
		{
			return 0;
		}
	}

	return 1;
}

bool AnimationDifferent( const Vector& startPos, const Quaternion& startqRot, const Vector& pos, const Quaternion& qrot )
{
	if ( !VectorCompareEpsilon( startPos, pos, 0.01 ) )
		return true;
	if ( !QuaternionCompare( startqRot, qrot ) )
		return true;

	return false;
}

bool BoneHasAnimation( const char *pName )
{
	bool first = true;
	Vector pos;
	//RadianEuler rot;
	Quaternion qrot;

	if ( !g_numani )
		return false;

	int globalIndex = findGlobalBone( pName );

	// don't check root bones for animation
	if (globalIndex >= 0 && g_bonetable[globalIndex].parent == -1)
		return true;

	// find used bones per g_model
	for (int i = 0; i < g_numani; i++)
	{
		s_source_t *psource = g_panimation[i]->source;
		const char *pAnimationName = g_panimation[i]->animationname;
		s_sourceanim_t *pSourceAnim = FindSourceAnim( psource, pAnimationName );

		int boneIndex = FindLocalBoneNamed(psource, pName);

		// not in this source?
		if (boneIndex < 0)
			continue;

		// this is not right, but enough of the bones are moved unintentionally between
		// animations that I put this in to catch them.
		first = true;
		int n = g_panimation[i]->startframe - pSourceAnim->startframe;
		// printf("%s %d:%d\n", g_panimation[i]->filename, g_panimation[i]->startframe, psource->startframe );
		for (int j = 0; j < g_panimation[i]->numframes; j++)
		{
			if ( first )
			{
				VectorCopy( pSourceAnim->rawanim[j+n][boneIndex].pos, pos );
				QuaternionCopy( pSourceAnim->rawanim[j+n][boneIndex].qrot, qrot );
				first = false;
			}
			else
			{
				if ( AnimationDifferent( pos, qrot, pSourceAnim->rawanim[j+n][boneIndex].pos, pSourceAnim->rawanim[j+n][boneIndex].qrot ) )
					return true;
			}
		}
	}
	return false;
}

bool BoneIsBonemerge( char const *pname )
{
	for (int k = 0; k < g_BoneMerge.Count(); k++)
	{
		if ( !stricmp( g_BoneMerge[k].bonename, pname ) )
		{
			return true;
		}
	}
	return false;
}

bool BoneShouldAlwaysSetup( char const *pname )
{
	for (int k = 0; k < g_BoneAlwaysSetup.Count(); k++)
	{
		if ( !stricmp( g_BoneAlwaysSetup[k].bonename, pname ) )
		{
			return true;
		}
	}
	return false;
}

bool BoneHasAttachments( char const *pname )
{
	for (int k = 0; k < g_numattachments; k++)
	{
		if ( !stricmp( g_attachment[k].bonename, pname ) )
		{
			return true;
		}
	}
	return false;
}

bool BoneIsProcedural( char const *pname )
{
	int k;

	for (k = 0; k < g_numaxisinterpbones; k++)
	{
		if (! stricmp( g_axisinterpbones[k].bonename, pname ) )
		{
			return true;
		}
	}

	for (k = 0; k < g_numquatinterpbones; k++)
	{
		if (IsGlobalBoneXSI( g_quatinterpbones[k].bonename, pname ) )
		{
			return true;
		}
	}

	for (k = 0; k < g_numaimatbones; k++)
	{
		if (IsGlobalBoneXSI( g_aimatbones[k].bonename, pname ) )
		{
			return true;
		}
	}

	for (k = 0; k < g_numjigglebones; k++)
	{
		if (! stricmp( g_jigglebones[k].bonename, pname ) )
		{
			return true;
		}
	}

	for ( k = 0; k < g_twistbones.Count(); ++k )
	{
		for ( int i = 0; i < g_twistbones[k].m_twistBoneTargets.Count(); ++i )
		{
			if ( IsGlobalBoneXSI( g_twistbones[k].m_twistBoneTargets[i].m_szBoneName, pname ) )
			{
				return true;
			}
		}
	}

	return false;
}


bool BoneIsIK( char const *pname )
{
	int k;

	// tag bones used by ikchains 
	for (k = 0; k < g_numikchains; k++)
	{
		if ( !stricmp( g_ikchain[k].bonename, pname ) )
		{
			return true;
		}
	}

	return false;
}

bool BoneShouldCollapse( char const *pname )
{
	int k;

	for (k = 0; k < g_collapse.Count(); k++)
	{
		if (stricmp( g_collapse[k], pname ) == 0)
		{
			return true;
		}
	}

	return ( !BoneHasAnimation( pname ) && !BoneIsProcedural( pname ) && !BoneIsIK( pname ) && !BoneHasAttachments( pname ) && !BoneIsBonemerge( pname ) );
}

//-----------------------------------------------------------------------------
// Purpose: Collapse vertex assignments up to parent on bones that are not needed
//			This can optimize a model substantially if the animator is using
//			lots of helper bones with no animation.
//-----------------------------------------------------------------------------
void CollapseBones( void )
{
	int j, k;
	int count = 0;

	for (k = 0; k < g_numbones; k++)
	{
		if ( g_bonetable[k].bDontCollapse )
			continue;

		int sBoneFlags = g_bonetable[k].flags;

		char szBoneReport[512] = "";
		V_strcat_safe( szBoneReport, " [" );
		V_strcat_safe( szBoneReport, g_bonetable[k].name );
		
		if( sBoneFlags & BONE_USED_BY_ANYTHING )
		{
			V_strcat_safe( szBoneReport, "]\t\tflags: " );

			if( sBoneFlags & BONE_USED_BY_ATTACHMENT )
				V_strcat_safe( szBoneReport, "attachments, " );

			if( sBoneFlags & BONE_USED_BY_HITBOX )
				V_strcat_safe( szBoneReport, "hitboxes, " );

			if( sBoneFlags & BONE_USED_BY_BONE_MERGE )
				V_strcat_safe( szBoneReport, "bonemerges, " );

			if( sBoneFlags & BONE_USED_BY_VERTEX_LOD0 )
				V_strcat_safe( szBoneReport, "lod0, " );

			if( sBoneFlags & BONE_USED_BY_VERTEX_LOD1 )
				V_strcat_safe( szBoneReport, "lod1, " );

			if( sBoneFlags & BONE_USED_BY_VERTEX_LOD2 )
				V_strcat_safe( szBoneReport, "lod2, " );

			if( sBoneFlags & BONE_USED_BY_VERTEX_LOD3 )
				V_strcat_safe( szBoneReport, "lod3, " );

			if( sBoneFlags & BONE_USED_BY_VERTEX_LOD4 )
				V_strcat_safe( szBoneReport, "lod4, " );

			if( sBoneFlags & BONE_USED_BY_VERTEX_LOD5 )
				V_strcat_safe( szBoneReport, "lod5, " );

			if( sBoneFlags & BONE_USED_BY_VERTEX_LOD6 )
				V_strcat_safe( szBoneReport, "lod6, " );

			if( sBoneFlags & BONE_USED_BY_VERTEX_LOD7 )
				V_strcat_safe( szBoneReport, "lod7, " );

			if( sBoneFlags & BONE_ALWAYS_SETUP )
				V_strcat_safe( szBoneReport, "alwayssetup, " );
			
		}
		else
		{
			V_strcat_safe( szBoneReport, "] is unused." );
		}
		
		// if it's being used by something other than a vertex, collapse it.
		if ( ((g_bonetable[k].flags & BONE_USED_BY_VERTEX_MASK) != 0) || !BoneShouldCollapse( g_bonetable[k].name ) )
		{
			if (g_collapse_bones_message)
			{
				Msg("[%08x] [keeping]    %s \n", sBoneFlags, szBoneReport );
			}
			continue;
		}

		count++;

		if( g_collapse_bones_message )
		{
			Msg("[%08x] [collapsing] %s \n", sBoneFlags, szBoneReport );
		}

		g_numbones--;
		int m = g_bonetable[k].parent;

		for (j = k; j < g_numbones; j++)
		{
			g_bonetable[j] = g_bonetable[j+1];
			if (g_bonetable[j].parent == k)
			{
				g_bonetable[j].parent = m;
			} 
			else if (g_bonetable[j].parent >= k)
			{
				g_bonetable[j].parent = g_bonetable[j].parent - 1;
			}
		}
		k--;
	}

	if( !g_quiet && count)
	{
		Msg("Collapsed %d bones\n", count );
	}
}


//-----------------------------------------------------------------------------
// Purpose: replace all animation, rotation and translation, etc. with a single bone
//-----------------------------------------------------------------------------
void MakeStaticProp()
{
	int i, j, k;
	matrix3x4_t rotated;
	Vector tmp;

	AngleMatrix( g_defaultrotation, rotated );
	VectorTransform( -g_defaultadjust, rotated, tmp );
	PositionMatrix( tmp, rotated );

	// FIXME: missing attachment point recalcs!

	// replace bone 0 with "static_prop" bone and attach everything to it.
	for (i = 0; i < g_numsources; i++)
	{
		s_source_t *psource = g_source[i];

		strcpy( psource->localBone[0].name, "static_prop" );
		psource->localBone[0].parent = -1;

		for (k = 1; k < psource->numbones; k++)
		{
			psource->localBone[k].parent = -1;
		}

		Vector mins, maxs;
		ClearBounds( mins, maxs );

		for (j = 0; j < psource->numvertices; j++)
		{
			for (k = 0; k < psource->vertex[j].boneweight.numbones; k++)
			{
				// attach everything to root
				psource->vertex[j].boneweight.bone[k] = 0;
			}

			// **shift everything into identity space**
			// position
			Vector tmp;
			VectorTransform( psource->vertex[j].position, rotated, tmp );
			VectorCopy( tmp, psource->vertex[j].position );

			// normal
			VectorRotate( psource->vertex[j].normal, rotated, tmp );
			VectorCopy( tmp, psource->vertex[j].normal );

			// tangentS
			VectorRotate( psource->vertex[j].tangentS.AsVector3D(), rotated, tmp );
			VectorCopy( tmp, psource->vertex[j].tangentS.AsVector3D() );

			// incrementally compute identity space bbox
			AddPointToBounds( psource->vertex[j].position, mins, maxs );
		}

		if ( g_centerstaticprop )
		{
			const char *pAttachmentName = "placementOrigin";
			bool bFound = false;
			for ( k = 0; k < g_numattachments; k++ )
			{
				if ( !Q_stricmp( g_attachment[k].name, pAttachmentName ) )
				{
					bFound = true;
					break;
				}
			}

			if ( !bFound )
			{
				g_PropCenterOffset = -0.5f * (mins + maxs);
			}

			for ( j = 0; j < psource->numvertices; j++ )
			{
				psource->vertex[j].position += g_PropCenterOffset;
			}

			if ( !bFound )
			{
				// now add an attachment point to store this offset
				Q_strncpy( g_attachment[g_numattachments].name, pAttachmentName, sizeof(g_attachment[g_numattachments].name) );
				Q_strncpy( g_attachment[g_numattachments].bonename, "static_prop", sizeof(g_attachment[g_numattachments].name) );
				g_attachment[g_numattachments].bone = 0;
				g_attachment[g_numattachments].type = 0;
				AngleMatrix( vec3_angle, g_PropCenterOffset, g_attachment[g_numattachments].local );
				g_numattachments++;
			}
		}

		// force the animation to be identity
		s_sourceanim_t *pSourceAnim = FindSourceAnim( psource, "BindPose" );
		if ( pSourceAnim )
		{
			pSourceAnim->rawanim[0][0].pos = Vector( 0, 0, 0 );
			//pSourceAnim->rawanim[0][0].rot = RadianEuler( 0, 0, 0 );
			pSourceAnim->rawanim[0][0].qrot = Quaternion(0,0,0,1);

			// make it all a single frame animation
			pSourceAnim->numframes = 1;
			pSourceAnim->startframe = 0;
			pSourceAnim->endframe = 1;
		}
	
		// make an identity boneToPose transform
		AngleMatrix( QAngle( 0, 0, 0 ), psource->boneToPose[0] );
	}

	// throw away all animations
	g_numani = 1;
	g_panimation[0]->numframes = 1;
	g_panimation[0]->startframe = 0;
	g_panimation[0]->endframe = 1;
	Q_strncpy( g_panimation[0]->animationname, "BindPose", sizeof(g_panimation[0]->animationname) );
	g_panimation[0]->rotation = RadianEuler( 0, 0, 0 );
	g_panimation[0]->adjust = Vector( 0, 0, 0 );

	// throw away all vertex animations
	g_numflexkeys = 0;
	g_defaultflexkey = NULL;

	// Recalc attachment points:
	for( i = 0; i < g_numattachments; i++ )
	{
		if( g_centerstaticprop && ( i == g_numattachments - 1 ) )
			continue;
		
		ConcatTransforms( rotated, g_attachment[i].local, g_attachment[i].local );

		Q_strncpy( g_attachment[i].bonename, "static_prop", sizeof(g_attachment[i].name) );
		g_attachment[i].bone = 0;
		g_attachment[i].type = 0;
	}
}


//-----------------------------------------------------------------------------
// Marks the boneref all the way up the bone hierarchy
//-----------------------------------------------------------------------------
static void UpdateBonerefRecursive( s_source_t *psource, int nBoneIndex, int nFlags )
{
	if ( nFlags == 0 )
		return;

	psource->boneref[nBoneIndex] |= nFlags;

	// Chain the flag up the parent
	int n = psource->localBone[nBoneIndex].parent;
	while (n != -1)
	{
		psource->boneref[n] |= psource->boneref[nBoneIndex];
		n = psource->localBone[n].parent;
	}
}


//-----------------------------------------------------------------------------
// Purpose: Returns the axis of the bone after remapping.  Axis 0:X, 1:Y, 2:Z
//          If the bone has a parent, axis is returned as is, if bone does not
//          have a parent then the $upaxis determines how the axes are mapped.
//			Only $upaxis Y is supported (see comment in Cmd_UpAxis).
//-----------------------------------------------------------------------------
int GetRemappedBoneAxis( int nBoneIndex, int nAxis )
{
	if ( nBoneIndex < 0 || nBoneIndex >= g_numbones )
		return nAxis;

	if ( g_bonetable[nBoneIndex].parent >= 0 )
		return nAxis;

	// Y Up
	if ( g_defaultrotation.x == static_cast< float >( M_PI / 2.0f ) && g_defaultrotation.y == 0.0f && g_defaultrotation.z == static_cast< float >( M_PI / 2.0f ) )
	{
		static const int nAxisMap[3] = { 1, 2, 0 };
		return nAxisMap[ nAxis ];
	}

	// Default Z Up
	return nAxis;
}


//-----------------------------------------------------------------------------
// Purpose: Map the flex driver bones to the global bone table
//          Also cleans up any that do not match to a global bone
//-----------------------------------------------------------------------------
void MapFlexDriveBonesToGlobalBoneTable()
{
	CDmeBoneFlexDriverList *pDmeBoneFlexDriverList = GetElement< CDmeBoneFlexDriverList >( g_hDmeBoneFlexDriverList );
	if ( !pDmeBoneFlexDriverList )
		return;

	// Loop backwards so we can remove elements as we go
	for ( int i = pDmeBoneFlexDriverList->m_eBoneFlexDriverList.Count() - 1; i >= 0; --i )
	{
		CDmeBoneFlexDriver *pDmeBoneFlexDriver = pDmeBoneFlexDriverList->m_eBoneFlexDriverList[i];
		if ( !pDmeBoneFlexDriver )
		{
			pDmeBoneFlexDriverList->m_eBoneFlexDriverList.Remove( i );
			continue;
		}

		for ( int j = 0; j < g_numbones; ++j )
		{
			if ( !Q_stricmp( g_bonetable[j].name, pDmeBoneFlexDriver->m_sBoneName.Get() ) )
			{
				if ( g_bonetable[j].flags & BONE_ALWAYS_PROCEDURAL )
				{
					MdlWarning( "DmeBoneFlexDriver Bone: %s is marked procedural, Ignoring flex drivers\n", pDmeBoneFlexDriver->m_sBoneName.Get() );
					pDmeBoneFlexDriverList->m_eBoneFlexDriverList.Remove( i );
					pDmeBoneFlexDriver = NULL;
				}

				pDmeBoneFlexDriver->SetValue( "__boneIndex", j );
				// Map the axis for Y up stuff
				for ( int k = 0; k < pDmeBoneFlexDriver->m_eControlList.Count(); ++k )
				{
					pDmeBoneFlexDriver->m_eControlList[k]->m_nBoneComponent = GetRemappedBoneAxis( j, pDmeBoneFlexDriver->m_eControlList[k]->m_nBoneComponent );
				}
				break;
			}
		}

		// Was removed because it was referencing a procedural bone
		if ( !pDmeBoneFlexDriver )
			continue;

		CDmAttribute *pBoneIndexAttr = pDmeBoneFlexDriver->GetAttribute( "__boneIndex" );
		if ( pBoneIndexAttr )
		{
			pBoneIndexAttr->AddFlag( FATTRIB_DONTSAVE );
		}
		else
		{
			MdlWarning( "DmeBoneFlexDriver Bone: %s - No Bone Found With That Name, Ignoring\n", pDmeBoneFlexDriver->m_sBoneName.Get() );
			pDmeBoneFlexDriverList->m_eBoneFlexDriverList.Remove( i );
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: Tag bones in the specified source that are used as a bone flex driver
//          Also cleans up any empty bone flex driver elements
//			Also tags the DmeBoneFlexDriverControl with 
//-----------------------------------------------------------------------------
void TagFlexDriverBones( s_source_t *pSource )
{
	CDmeBoneFlexDriverList *pDmeBoneFlexDriverList = GetElement< CDmeBoneFlexDriverList >( g_hDmeBoneFlexDriverList );
	if ( !pDmeBoneFlexDriverList )
		return;

	// Loop backwards so we can remove elements as we go
	for ( int i = pDmeBoneFlexDriverList->m_eBoneFlexDriverList.Count() - 1; i >= 0; --i )
	{
		CDmeBoneFlexDriver *pDmeBoneFlexDriver = pDmeBoneFlexDriverList->m_eBoneFlexDriverList[i];
		if ( !pDmeBoneFlexDriver )
		{
			pDmeBoneFlexDriverList->m_eBoneFlexDriverList.Remove( i );
			continue;
		}

		for ( int j = pDmeBoneFlexDriver->m_eControlList.Count() - 1; j >= 0; --j )
		{
			CDmeBoneFlexDriverControl *pDmeBoneFlexDriverControl = pDmeBoneFlexDriver->m_eControlList[j];
			if ( !pDmeBoneFlexDriverControl )
			{
				pDmeBoneFlexDriver->m_eControlList.Remove( j );
				continue;
			}

			if ( pDmeBoneFlexDriverControl->m_nBoneComponent < STUDIO_BONE_FLEX_TX || pDmeBoneFlexDriverControl->m_nBoneComponent > STUDIO_BONE_FLEX_TZ )
			{
				MdlWarning( "DmeBoneFlexDriver Bone: %s - Flex Controlle: %s, Bone Component Out Of Range: %d [0-2], Ignoring\n", pDmeBoneFlexDriver->m_sBoneName.Get(), pDmeBoneFlexDriverControl->m_sFlexControllerName.Get(), pDmeBoneFlexDriverControl->m_nBoneComponent );
				pDmeBoneFlexDriver->m_eControlList.Remove( j );
				continue;
			}

			for ( int k = 0; k < g_numflexcontrollers; ++k )
			{
				if ( !Q_stricmp( g_flexcontroller[k].name, pDmeBoneFlexDriverControl->m_sFlexControllerName.Get() ) )
				{
					pDmeBoneFlexDriverControl->SetValue( "__flexControlIndex", k );
					break;
				}
			}

			if ( !pDmeBoneFlexDriverControl->HasAttribute( "__flexControlIndex" ) )
			{
				MdlWarning( "DmeBoneFlexDriver Bone: %s - No Flex Controller Named: %s, Ignoring\n", pDmeBoneFlexDriver->m_sBoneName.Get(), pDmeBoneFlexDriverControl->m_sFlexControllerName.Get() );
				pDmeBoneFlexDriver->m_eControlList.Remove( j );
			}
		}

		if ( pDmeBoneFlexDriver->m_eControlList.Count() <= 0 )
		{
			MdlWarning( "DmeBoneFlexDriver Bone: %s - No Flex Controllers Defined, Ignoring\n", pDmeBoneFlexDriver->m_sBoneName.Get() );
			pDmeBoneFlexDriverList->m_eBoneFlexDriverList.Remove( i );
			continue;
		}

		for ( int j = 0; j < pSource->numbones; ++j )
		{
			if ( !Q_stricmp( pSource->localBone[j].name, pDmeBoneFlexDriver->m_sBoneName.Get() ) )
			{
				// Mark used by all LODs
				pSource->boneflags[j] |= BONE_USED_BY_VERTEX_MASK;
			}
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: set "boneref" for all the source bones used by vertices, attachments, eyeballs, etc.
//-----------------------------------------------------------------------------
void TagUsedBones( )
{
	int i, j, k;
	int n;

	// find used bones per g_model
	for (i = 0; i < g_numsources; i++)
	{
		s_source_t *psource = g_source[i];

		for (k = 0; k < MAXSTUDIOSRCBONES; k++)
		{
			psource->boneflags[k] = 0;
			psource->boneref[k] = 0;
		}

		if (!psource->isActiveModel)
			continue;

		// printf("active: %s\n", psource->filename );
		for (j = 0; j < psource->numvertices; j++)
		{
			for (k = 0; k < psource->vertex[j].boneweight.numbones; k++)
			{
				psource->boneflags[psource->vertex[j].boneweight.bone[k]] |= BONE_USED_BY_VERTEX_LOD0;
			}
		}
	}

	// find used bones per g_model
	for (i = 0; i < g_numsources; i++)
	{
		s_source_t *psource = g_source[i];

		// FIXME: this is in the wrong place.  The attachment may be rigid and it never defined in a reference file
		for (k = 0; k < g_numattachments; k++)
		{
			for (j = 0; j < psource->numbones; j++)
			{
				if ( !stricmp( g_attachment[k].bonename, psource->localBone[j].name ) )
				{
					// this bone is a keeper with or without associated vertices
					// because an attachment point depends on it.
					if (g_attachment[k].type & IS_RIGID)
					{
						bool bBoneFlagged = false;

						for (n = j; n != -1; n = psource->localBone[n].parent)
						{
							if ( psource->boneflags[n] & BONE_USED_BY_VERTEX_LOD0 )
							{
								psource->boneflags[n] |= BONE_USED_BY_ATTACHMENT;
								bBoneFlagged = true;
								break;
							}

							// Check to see if the ancestor bone is in g_importbones because
							// all bones in g_importbones are kept
							for ( int ib = 0; ib < g_numimportbones; ++ib )
							{
								if ( !Q_stricmp( psource->localBone[n].name, g_importbone[ib].name ) )
								{
									psource->boneflags[n] |= BONE_USED_BY_ATTACHMENT;
									bBoneFlagged = true;
								}
							}
						}

						// If nothing was flagged, that means that no ancestor bone
						// in the hierarchy is used by VERTEX_LOD0 so tag the bone
						// itself because need to make sure at least one bone is
						// left for the attachment to attach to
						if ( !bBoneFlagged )
						{
							psource->boneflags[j] |= BONE_USED_BY_ATTACHMENT;
						}
					}
					else
					{
						psource->boneflags[j] |= BONE_USED_BY_ATTACHMENT;
					}
				}
			}
		}

		for (k = 0; k < g_numikchains; k++)
		{
			for (j = 0; j < psource->numbones; j++)
			{
				if ( !stricmp( g_ikchain[k].bonename, psource->localBone[j].name ) )
				{
					// this bone is a keeper with or without associated vertices
					// because a ikchain depends on it.
					psource->boneflags[j] |= BONE_USED_BY_ATTACHMENT;
				}
			}
		}

		for (k = 0; k < g_nummouths; k++)
		{
			for (j = 0; j < psource->numbones; j++)
			{
				if ( !stricmp( g_mouth[k].bonename, psource->localBone[j].name ) )
				{
					// this bone is a keeper with or without associated vertices
					// because a mouth shader depends on it.
					psource->boneflags[j] |= BONE_USED_BY_ATTACHMENT;
				}
			}
		}

		// Tag all bones marked as being used by bonemerge
		int nBoneMergeCount = g_BoneMerge.Count(); 
		for ( k = 0; k < nBoneMergeCount; ++k )
		{
			for ( j = 0; j < psource->numbones; j++ )
			{
				if ( stricmp( g_BoneMerge[k].bonename, psource->localBone[j].name ) )
					continue;

				psource->boneflags[j] |= BONE_USED_BY_BONE_MERGE;
			}
		}

		// Tag bones used as bone flex drivers, these need to be client side only
		TagFlexDriverBones( psource );

		// NOTE: This must come last; after all flags have been set!
		// tag bonerefs as being used the union of the boneflags all their children
		for (k = 0; k < psource->numbones; k++)
		{
			UpdateBonerefRecursive( psource, k, psource->boneflags[k] );
		}

		// Tag all bones marked as being used by alwayssetup
		// NOTE these are intentionally added without respect to parents, 
		// because they are intended to be used on data-driving bones that are aren't 
		// necessarily moving vertices or part of a hierarchy. They are NOT guaranteed 
		// to be positioned correctly relative to their parents!!!
		int nBoneAlwaysSetupCount = g_BoneAlwaysSetup.Count(); 
		for ( k = 0; k < nBoneAlwaysSetupCount; ++k )
		{
			for ( j = 0; j < psource->numbones; j++ )
			{
				if ( stricmp( g_BoneAlwaysSetup[k].bonename, psource->localBone[j].name ) )
					continue;

				psource->boneflags[j] |= BONE_ALWAYS_SETUP;
			}
		}

		// don't add more flags here! Add them up above the UpdateBonerefRecursive call, so they get propagated up their parents!

	}

	// tag all eyeball bones
	for (i = 0; i < g_nummodelsbeforeLOD; i++)
	{
		s_source_t *psource = g_model[i]->source;
		for (k = 0; k < g_model[i]->numeyeballs; k++)
		{
			psource->boneref[g_model[i]->eyeball[k].bone] |= BONE_USED_BY_ATTACHMENT;
		}
	}

}


//-----------------------------------------------------------------------------
// Purpose: change the names in the source files for bones that max auto-renamed on us
//-----------------------------------------------------------------------------
void RenameBones( )
{
	int i, j, k;

	// rename source bones if needed
	for (i = 0; i < g_numsources; i++)
	{
		for (j = 0; j < g_source[i]->numbones; j++)
		{

			for (k = 0; k < g_numRenameBoneSubstr; k++)
			{
				char temp[MAXSTUDIONAME];
				if ( V_stristr( g_source[i]->localBone[j].name, g_szRenameBoneSubstr[k].from ) && !V_stristr( g_source[i]->localBone[j].name, g_szRenameBoneSubstr[k].to ) )
				{
					V_strcpy( temp, g_source[i]->localBone[j].name );
					V_StrSubst( temp, g_szRenameBoneSubstr[k].from, g_szRenameBoneSubstr[k].to, g_source[i]->localBone[j].name, sizeof( g_source[i]->localBone[j].name ) );
					continue;
				}
			}

			for (k = 0; k < g_numrenamedbones; k++)
			{
				if (!stricmp( g_source[i]->localBone[j].name, g_renamedbone[k].from))
				{
					strcpy( g_source[i]->localBone[j].name, g_renamedbone[k].to );
					break;
				}
			}
		}
	}
}


const char *RenameBone( const char *pName )
{

	// check for prefixes to strip
	for ( int k = 0; k < g_numStripBonePrefixes; k++)
	{
		if ( !Q_strncmp( pName, g_szStripBonePrefix[k], V_strlen( g_szStripBonePrefix[k] ) ) )
		{
			//Msg("Stripping bone prefix %s from %s\n", g_szStripBonePrefix[k], pName );

			// recurse in case we're removing more than one prefix? Not sure if this is necessary, but maybe you want to remove "Alpha" then "Beta" from bone "AlphaBetaCharlie" ?
			return RenameBone( pName + V_strlen( g_szStripBonePrefix[k] ) ); 
		}
	}

	for ( int k = 0; k < g_numrenamedbones; k++)
	{
		if ( !Q_stricmp( pName, g_renamedbone[k].from ) )
			return g_renamedbone[k].to;
	}
	return pName;
}

void InsertPredefinedBones( bool bUnlocked )
{
	int i, k;

	for (i = 0; i < g_numimportbones; i++)
	{
		if ( g_importbone[i].bUnlocked != bUnlocked )
			continue;

		k = findGlobalBone( g_importbone[i].name );
		if (k == -1)
		{
			k = g_numbones;
			strcpyn( g_bonetable[k].name, g_importbone[i].name );
			if ( strlen( g_importbone[i].parent ) == 0 )
			{
				g_bonetable[k].parent = -1;
			}
			else
			{
				// FIXME: This won't work if the imported bone refers to
				// another imported bone which is further along in the list
				g_bonetable[k].parent = findGlobalBone( g_importbone[i].parent );
				if ( g_bonetable[k].parent == -1 )
				{
					Warning("Imported bone %s tried to access parent bone %s and failed!\n",
						g_importbone[i].name, g_importbone[i].parent );
				}
			}
			g_bonetable[k].bPreDefined = true;
			g_bonetable[k].rawLocal = g_importbone[i].rawLocal;
			g_bonetable[k].rawLocalOriginal = g_bonetable[k].rawLocal;
			g_numbones++;
		}
		g_bonetable[k].bDontCollapse = true;
		g_bonetable[k].srcRealign = g_importbone[i].srcRealign;
		g_bonetable[k].bPreAligned = g_importbone[i].bPreAligned;
	}

	// ensure bonemerged bones are tagged
	for ( i = 0; i < g_numbones; i++ )
	{
		for ( k = 0; k < g_BoneMerge.Count(); k++ )
		{
			if ( !(g_bonetable[i].flags & BONE_USED_BY_BONE_MERGE) && !stricmp( g_BoneMerge[k].bonename, g_bonetable[i].name ) )
			{
				g_bonetable[i].flags |= BONE_USED_BY_BONE_MERGE;
			}
		}
	}

	// ensure alwayssetup bones are tagged
	for ( i = 0; i < g_numbones; i++ )
	{
		for ( k = 0; k < g_BoneAlwaysSetup.Count(); k++ )
		{
			if ( !(g_bonetable[i].flags & BONE_ALWAYS_SETUP) && !stricmp( g_BoneAlwaysSetup[k].bonename, g_bonetable[i].name ) )
			{
				g_bonetable[i].flags |= BONE_ALWAYS_SETUP;
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: look through all the sources and build a table of used bones
//-----------------------------------------------------------------------------
int BuildGlobalBonetable( )
{
	int i, j, k, n;
	int	iError = 0;

	g_numbones = 0;

	for (i = 0; i < MAXSTUDIOSRCBONES; i++)
	{
		SetIdentityMatrix( g_bonetable[i].srcRealign );
	}

	InsertPredefinedBones( false );

	// union of all used bones
	for ( i = 0; i < g_numsources; i++ )
	{
		s_source_t *psource = g_source[i];

		// skip sources with no bones
		if (psource->numbones == 0)
			continue;

		matrix3x4_t srcBoneToWorld[MAXSTUDIOSRCBONES];
		s_sourceanim_t *pSourceAnim = FindSourceAnim( psource, "BindPose" );
		if ( !pSourceAnim )
		{
			pSourceAnim = &psource->m_Animations[0];
		}
		BuildRawTransforms( psource, pSourceAnim->animationname, 0, srcBoneToWorld );

		for ( j = 0; j < psource->numbones; j++ )
		{
			if ( g_collapse_bones_aggressive )
			{
				if ( psource->boneflags[j] == 0 )
					continue;
			}
			else
			{
				if ( psource->boneref[j] == 0 )
					continue;
			}

			k = findGlobalBone( psource->localBone[j].name );
			if (k == -1)
			{

				// create new bone
				k = g_numbones;
				strcpyn( g_bonetable[k].name, psource->localBone[j].name );
				if ((n = psource->localBone[j].parent) != -1)
					g_bonetable[k].parent		= findGlobalBone( psource->localBone[n].name );
				else
					g_bonetable[k].parent		= -1;
				g_bonetable[k].bonecontroller	= 0;
				g_bonetable[k].flags			= psource->boneflags[j];

				if ( g_bonetable[k].parent == -1 || !g_bonetable[g_bonetable[k].parent].bPreAligned )
				{
					QuaternionMatrix( pSourceAnim->rawanim[0][j].qrot, pSourceAnim->rawanim[0][j].pos, g_bonetable[k].rawLocal );
					g_bonetable[k].rawLocalOriginal = g_bonetable[k].rawLocal;
				}
				else
				{
					// convert the local relative position into a realigned relative position
					matrix3x4_t srcParentBoneToWorld;
					ConcatTransforms( srcBoneToWorld[n], g_bonetable[g_bonetable[k].parent].srcRealign, srcParentBoneToWorld );
					matrix3x4_t invSrcParentBoneToWorld;
					MatrixInvert( srcParentBoneToWorld, invSrcParentBoneToWorld );
					ConcatTransforms( invSrcParentBoneToWorld, srcBoneToWorld[j], g_bonetable[k].rawLocal );
				}

				g_bonetable[k].boneToPose.Invalidate();

				for (int n = 0; n < g_numimportbones; n++)
				{
					if (!Q_stricmp( g_bonetable[k].name, g_importbone[n].name ))
					{
						g_bonetable[k].bDontCollapse = true;
					}
				}

				// printf("%d : %s (%s)\n", k, g_bonetable[k].name, g_bonetable[g_bonetable[k].parent].name );
				g_numbones++;
				continue;
			}
			
			// accumlate flags
			g_bonetable[k].flags |= psource->boneflags[j];
		}
	}

	InsertPredefinedBones( true );

	return iError;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

void BuildGlobalBoneToPose( )
{
	int k;

	// build reference pose
	for (k = 0; k < g_numbones; k++)
	{
		if (g_bonetable[k].parent == -1)
		{
			MatrixCopy( g_bonetable[k].rawLocal, g_bonetable[k].boneToPose );
		}
		else
		{
			ConcatTransforms (g_bonetable[g_bonetable[k].parent].boneToPose, g_bonetable[k].rawLocal, g_bonetable[k].boneToPose);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

void RebuildLocalPose( )
{
	int k;

	matrix3x4_t boneToPose[MAXSTUDIOBONES];

	// build reference pose
	for (k = 0; k < g_numbones; k++)
	{
		MatrixCopy( g_bonetable[k].boneToPose, boneToPose[k] );
	}

	matrix3x4_t poseToBone[MAXSTUDIOBONES];

	// rebuild local pose
	for (k = 0; k < g_numbones; k++)
	{
		if (g_bonetable[k].parent == -1)
		{
			MatrixCopy( boneToPose[k], g_bonetable[k].rawLocal );
		}
		else
		{
			ConcatTransforms (poseToBone[g_bonetable[k].parent], boneToPose[k], g_bonetable[k].rawLocal );
		}
		MatrixQuaternion( g_bonetable[k].rawLocal, g_bonetable[k].qrot, g_bonetable[k].pos );
		MatrixCopy( boneToPose[k], g_bonetable[k].boneToPose );
		MatrixInvert( boneToPose[k], poseToBone[k] );

		// printf("%d \"%s\" %d\n", k, g_bonetable[k].name, g_bonetable[k].parent );
	}
	//exit(0);

}


//-----------------------------------------------------------------------------
// Purpose: attach bones to different parents if needed
//-----------------------------------------------------------------------------

void EnforceHierarchy( )
{
	int i, j, k;

	// force changes to hierarchy
	for (i = 0; i < g_numforcedhierarchy; i++)
	{
		j = findGlobalBone( g_forcedhierarchy[i].parentname );
		k = findGlobalBone( g_forcedhierarchy[i].childname );

		if (j == -1 && strlen( g_forcedhierarchy[i].parentname ) > 0 )
		{
			MdlError( "unknown bone: \"%s\" in forced hierarchy\n", g_forcedhierarchy[i].parentname );
		}
		if (k == -1)
		{
			MdlError( "unknown bone: \"%s\" in forced hierarchy\n", g_forcedhierarchy[i].childname );
		}
		
		/*
		if (j > k)
		{
			MdlError( "parent \"%s\" declared after child \"%s\" in forced hierarchy\n", g_forcedhierarchy[i].parentname, g_forcedhierarchy[i].childname );
		}
		*/

		/*
		if (strlen(g_forcedhierarchy[i].subparentname) != 0)
		{
			int n, m;

			m = findGlobalBone( g_forcedhierarchy[i].subparentname );
			if (m != -1)
			{
				MdlError( "inserted bone \"%s\" matches name of existing bone in hierarchy\n", g_forcedhierarchy[i].parentname, g_forcedhierarchy[i].subparentname );
			}

			printf("inserting bone \"%s\"\n", g_forcedhierarchy[i].subparentname );

			// shift the bone list up
			for (n = g_numbones; n > k; n--)
			{
				g_bonetable[n] = g_bonetable[n-1];
				if (g_bonetable[n].parent >= k)
				{
					g_bonetable[n].parent = g_bonetable[n].parent + 1;
				}
				MatrixCopy( boneToPose[n-1], boneToPose[n] );
			}
			g_numbones++;

			// add the bone
			strcpy( g_bonetable[k].name, g_forcedhierarchy[i].subparentname );
			g_bonetable[k].parent = j;
			g_bonetable[k].split = true;
			g_bonetable[k+1].parent = k;

			// split the bone
			Quaternion q1, q2;
			Vector p;
			MatrixAngles( boneToPose[k], q1, p );  // FIXME: badly named!

			// !!!!
			// QuaternionScale( q1, 0.5, q2 );
			// q2.Init( 0, 0, 0, 1 );
			// AngleQuaternion( QAngle( 0, 0, 0 ), q2 );
			//QuaternionMatrix( q2, p, boneToPose[k] );
			QuaternionMatrix( q1, p, boneToPose[k] );
			QuaternionMatrix( q1, p, boneToPose[k+1] );
		}
		else
		*/
		{
			g_bonetable[k].parent = j;
		}
	}


	// resort hierarchy
	bool bSort = true;
	int count = 0;

	while (bSort) 
	{
		count++;
		bSort = false;
		for (i = 0; i < g_numbones; i++)
		{
			if (g_bonetable[i].parent > i)
			{
				// swap
				j = g_bonetable[i].parent;
				s_bonetable_t tmp;
				tmp = g_bonetable[i];
				g_bonetable[i] = g_bonetable[j];
				g_bonetable[j] = tmp;

				// relink parents
				for (k = i; k < g_numbones; k++)
				{
					if (g_bonetable[k].parent == i)
					{
						g_bonetable[k].parent = j;
					} 
					else if (g_bonetable[k].parent == j)
					{
						g_bonetable[k].parent = i;
					}
				}

				bSort = true;
			}
		}
		if (count > 1000)
		{
			MdlError( "Circular bone hierarchy\n");
		}
	}
}



//-----------------------------------------------------------------------------
// Purpose: Find constraint bones and tag for inclusion
//-----------------------------------------------------------------------------
static void TagConstraintBones()
{
	// Iterate backwards so invalid elements can be removed
	for ( int i = g_constraintBones.Count() - 1; i >= 0; --i )
	{
		CConstraintBoneBase *pConstraintBone = g_constraintBones[i];
		if ( !pConstraintBone )
		{
			g_constraintBones.Remove( i );
			continue;
		}

		pConstraintBone->m_slave.m_nBone = findGlobalBone( pConstraintBone->m_slave.m_szBoneName );
		if ( pConstraintBone->m_slave.m_nBone < 0 )
		{
			g_constraintBones.Remove( i );
			continue;
		}

		g_bonetable[pConstraintBone->m_slave.m_nBone].flags |= BONE_ALWAYS_PROCEDURAL;

		for ( int j = pConstraintBone->m_targets.Count() - 1; j >= 0; --j )
		{
			s_constraintbonetarget_t &target = pConstraintBone->m_targets[j];
			target.m_nBone = findGlobalBone( target.m_szBoneName );
			if ( target.m_nBone < 0 )
			{
				pConstraintBone->m_targets.Remove( j );
			}
		}

		if ( pConstraintBone->m_targets.Count() <= 0 )
		{
			g_constraintBones.Remove( i );
		}

		CAimConstraint *pAimConstraint = dynamic_cast< CAimConstraint * >( pConstraintBone );
		if ( pAimConstraint )
		{
			pAimConstraint->m_nUpSpaceTargetBone = findGlobalBone( pAimConstraint->m_szUpSpaceTargetBone );
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: find procedural bones and tag for inclusion even if they don't animate
//-----------------------------------------------------------------------------

void TagProceduralBones( )
{
	int j;

	// look for AxisInterp bone definitions
	int numaxisinterpbones = 0;
	for (j = 0; j < g_numaxisinterpbones; j++)
	{
		g_axisinterpbones[j].bone = findGlobalBone( g_axisinterpbones[j].bonename );
		g_axisinterpbones[j].control = findGlobalBone( g_axisinterpbones[j].controlname );

		if (g_axisinterpbones[j].bone == -1) 
		{
			if (!g_quiet)
			{
				printf("axisinterpbone \"%s\" unused\n", g_axisinterpbones[j].bonename );
			}
			continue; // optimized out, don't complain
		}

		if (g_axisinterpbones[j].control == -1)
		{
			MdlError( "Missing control bone \"%s\" for procedural bone \"%s\"\n", g_axisinterpbones[j].bonename, g_axisinterpbones[j].controlname );
		}

		g_bonetable[g_axisinterpbones[j].bone].flags |= BONE_ALWAYS_PROCEDURAL; // ??? what about physics rules
		g_axisinterpbonemap[numaxisinterpbones++] = j;
	}
	g_numaxisinterpbones = numaxisinterpbones;

	// look for QuatInterp bone definitions
	int numquatinterpbones = 0;
	for (j = 0; j < g_numquatinterpbones; j++)
	{
		g_quatinterpbones[j].bone = findGlobalBoneXSI( g_quatinterpbones[j].bonename );
		g_quatinterpbones[j].control = findGlobalBoneXSI( g_quatinterpbones[j].controlname );

		if (g_quatinterpbones[j].bone == -1) 
		{
			if (!g_quiet && !g_bCreateMakefile )
			{
				printf("quatinterpbone \"%s\" unused\n", g_quatinterpbones[j].bonename );
			}
			continue; // optimized out, don't complain
		}

		if (g_quatinterpbones[j].control == -1)
		{
			MdlError( "Missing control bone \"%s\" for procedural bone \"%s\"\n", g_quatinterpbones[j].bonename, g_quatinterpbones[j].controlname );
		}

		g_bonetable[g_quatinterpbones[j].bone].flags |= BONE_ALWAYS_PROCEDURAL; // ??? what about physics rules
		g_quatinterpbonemap[numquatinterpbones++] = j;
	}
	g_numquatinterpbones = numquatinterpbones;
	// look for AimAt bone definitions
	int numaimatbones = 0;
	for (j = 0; j < g_numaimatbones; j++)
	{
		g_aimatbones[j].bone =		findGlobalBoneXSI( g_aimatbones[j].bonename );

		if (g_aimatbones[j].bone == -1) 
		{
			if (!g_quiet && !g_bCreateMakefile )
			{
				printf("<aimconstraint> \"%s\" unused\n", g_aimatbones[j].bonename );
			}
			continue; // optimized out, don't complain
		}

		g_aimatbones[j].parent =	findGlobalBoneXSI( g_aimatbones[j].parentname );

		if (g_aimatbones[j].parent == -1)
		{
			MdlError( "Missing parent control bone \"%s\" for procedural bone \"%s\"\n", g_aimatbones[j].parentname, g_aimatbones[j].bonename );
		}

		// Look for the aim bone as an attachment first

		g_aimatbones[j].aimAttach = -1;

		for ( int ai( 0 ); ai < g_numattachments; ++ai )
		{
			if ( strcmp( g_attachment[ ai ].name, g_aimatbones[j].aimname ) == 0 )
			{
				g_aimatbones[j].aimAttach = ai;
				break;
			}
		}

		if ( g_aimatbones[j].aimAttach == -1 )
		{
			g_aimatbones[j].aimBone = findGlobalBoneXSI( g_aimatbones[j].aimname );

			if ( g_aimatbones[j].aimBone == -1 )
			{
				MdlError( "Missing aim control attachment or bone \"%s\" for procedural bone \"%s\"\n",
					g_aimatbones[j].aimname, g_aimatbones[j].bonename );
			}
		}

		g_bonetable[g_aimatbones[j].bone].flags |= BONE_ALWAYS_PROCEDURAL; // ??? what about physics rules
		g_aimatbonemap[numaimatbones++] = j;
	}

	// look for Jiggle bone definitions
	int numjigglebones = 0;
	for (j = 0; j < g_numjigglebones; j++)
	{
		g_jigglebones[j].bone = findGlobalBone( g_jigglebones[j].bonename );

		if (g_jigglebones[j].bone == -1) 
		{
			if (!g_quiet)
			{
				printf("jigglebone \"%s\" unused\n", g_jigglebones[j].bonename );
			}
			continue; // optimized out, don't complain
		}

		g_bonetable[g_jigglebones[j].bone].flags |= BONE_ALWAYS_PROCEDURAL; // ??? what about physics rules
		g_jigglebonemap[numjigglebones++] = j;
	}
	g_numjigglebones = numjigglebones;

	// Look for twist bone defintions
	// Iterate backwards so we can remove invalid elements
	for ( j = g_twistbones.Count() - 1; j >= 0; --j )
	{
		CTwistBone &twistBone = g_twistbones.Element( j );

		twistBone.m_nParentBone = findGlobalBone( twistBone.m_szParentBoneName );
		if ( twistBone.m_nParentBone < 0 )
		{
			g_twistbones.Remove( j );
			continue;
		}

		twistBone.m_nChildBone = findGlobalBone( twistBone.m_szChildBoneName );
		if ( twistBone.m_nChildBone < 0 )
		{
			g_twistbones.Remove( j );
			continue;
		}

		for ( int k = twistBone.m_twistBoneTargets.Count() - 1; k >= 0; --k )
		{
			s_constraintbonetarget_t &twistBoneTarget = twistBone.m_twistBoneTargets[k];
			twistBoneTarget.m_nBone = findGlobalBone( twistBoneTarget.m_szBoneName );
			if ( twistBoneTarget.m_nBone < 0 )
			{
				twistBone.m_twistBoneTargets.Remove( k );
			}
			else
			{
				g_bonetable[twistBoneTarget.m_nBone].flags |= BONE_ALWAYS_PROCEDURAL;
			}
		}

		if ( twistBone.m_twistBoneTargets.Count() <= 0 )
		{
			g_twistbones.Remove( j );
		}
	}

	TagConstraintBones();
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void RealignBoneTranslation( Vector &vRealigned, int nBoneIndex, const Vector &vInput )
{
	matrix3x4a_t mIdentity;
	SetIdentityMatrix( mIdentity );

	if ( nBoneIndex < 0 || nBoneIndex >= MAXSTUDIOSRCBONES )
		return;

	s_bonetable_t *pBone = &g_bonetable[ nBoneIndex ];
	if ( !pBone )
		return;

	Vector vParentRealigned = vInput;

	const int nParentBoneIndex = pBone->parent;
	if ( nParentBoneIndex >= 0 && nBoneIndex < MAXSTUDIOSRCBONES )
	{
		s_bonetable_t *pParentBone = &g_bonetable[ nParentBoneIndex ];
		if ( pParentBone )
		{
			if ( !MatricesAreEqual( mIdentity, pParentBone->srcRealign ) )
			{
				QuaternionAligned qParentSrcRealign;
				MatrixQuaternion( pParentBone->srcRealign, qParentSrcRealign );

				QuaternionAligned qParentSrcRealignInv;
				QuaternionInvert( qParentSrcRealign, qParentSrcRealignInv );

				VectorRotate( vInput, qParentSrcRealignInv, vParentRealigned );
			}
		}
	}

	vRealigned = vParentRealigned;
}


//-----------------------------------------------------------------------------
// Purpose: Realign Orientation of a rotation on a bone
// Applies the inverse of the parent's srcRealign and then the it's srcRealign
// if not the identity
//-----------------------------------------------------------------------------
void RealignBoneQuaternion( Quaternion &qRealigned, int nBoneIndex, const Quaternion &qInput )
{
	matrix3x4a_t mIdentity;
	SetIdentityMatrix( mIdentity );

	if ( nBoneIndex < 0 || nBoneIndex >= MAXSTUDIOSRCBONES )
		return;

	s_bonetable_t *pBone = &g_bonetable[ nBoneIndex ];
	if ( !pBone )
		return;

	Quaternion qParentRealigned = qInput;

	const int nParentBoneIndex = pBone->parent;
	if ( nParentBoneIndex >= 0 && nBoneIndex < MAXSTUDIOSRCBONES )
	{
		s_bonetable_t *pParentBone = &g_bonetable[ nParentBoneIndex ];
		if ( pParentBone )
		{
			if ( !MatricesAreEqual( mIdentity, pParentBone->srcRealign ) )
			{
				matrix3x4a_t mSrcRealignInv;
				MatrixInvert( pParentBone->srcRealign, mSrcRealignInv );
				Quaternion qSrcRealignInv;
				MatrixQuaternion( mSrcRealignInv, qSrcRealignInv );
				QuaternionMult( qSrcRealignInv, qInput, qParentRealigned );
			}
		}
	}

	if ( !MatricesAreEqual( mIdentity, pBone->srcRealign ) )
	{
		Quaternion qSrcRealign;
		MatrixQuaternion( pBone->srcRealign, qSrcRealign );
		QuaternionMult( qParentRealigned, qSrcRealign, qRealigned );
	}
	else
	{
		qRealigned = qParentRealigned;
	}
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
static void RemapConstraintBones()
{
	const Quaternion qRot = Quaternion( g_defaultrotation );
	Vector vTmp;

	for ( int i = 0; i < g_constraintBones.Count(); ++i )
	{
		CConstraintBoneBase *pConstraintBone = g_constraintBones[i];
		if ( !pConstraintBone )
			continue;

		for ( int j = 0; j < pConstraintBone->m_targets.Count(); ++j )
		{
			s_constraintbonetarget_t &target = pConstraintBone->m_targets[j];
			const int nBoneIndex = findGlobalBone( target.m_szBoneName );
			if ( nBoneIndex < 0 || nBoneIndex != target.m_nBone )
			{
				MdlError( "<constraintbone> Can't find target bone \"%s\"\n", target.m_szBoneName );
			}

			if ( !dynamic_cast< CPointConstraint * >( pConstraintBone ) )
			{
				RealignBoneTranslation( target.m_vOffset, nBoneIndex, target.m_vOffset );
				RealignBoneQuaternion( target.m_qOffset, nBoneIndex, target.m_qOffset );
			}
			else
			{
				// target offsets are in world space...
			}
		}

		{
			s_constraintboneslave_t &slave = pConstraintBone->m_slave;

			const int nBoneIndex = findGlobalBone( slave.m_szBoneName );
			if ( nBoneIndex < 0 || nBoneIndex != slave.m_nBone )
			{
				MdlError( "<constraintbone> Can't find slave bone \"%s\"\n", slave.m_szBoneName );
			}

			s_bonetable_t *pBone = &g_bonetable[ nBoneIndex ];
			if ( pBone )
			{
				if ( pBone->parent < 0 )
				{
					// No parent
					VectorRotate( slave.m_vBaseTranslate, qRot, vTmp );
					slave.m_vBaseTranslate = vTmp;
				}
				else
				{
					RealignBoneTranslation( slave.m_vBaseTranslate, nBoneIndex, slave.m_vBaseTranslate );
				}

				RealignBoneQuaternion( slave.m_qBaseRotation, nBoneIndex, slave.m_qBaseRotation );
			}
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: convert original procedural bone info into correct values for existing skeleton
//-----------------------------------------------------------------------------
void RemapProceduralBones( )
{
	int j;

	// look for QuatInterp bone definitions
	for (j = 0; j < g_numquatinterpbones; j++)
	{
		s_quatinterpbone_t *pInterp = &g_quatinterpbones[g_quatinterpbonemap[j]];

		int origParent = findGlobalBoneXSI( pInterp->parentname );
		int origControlParent = findGlobalBoneXSI( pInterp->controlparentname );

		if (origParent == -1)
		{
			MdlError( "procedural bone \"%s\", can't find orig parent \"%s\"\n\n", pInterp->bonename, pInterp->parentname );
		}

		if (origControlParent == -1)
		{
			MdlError( "procedural bone \"%s\", can't find control parent \"%s\n\n", pInterp->bonename, pInterp->controlparentname );
		}

		if ( g_bonetable[pInterp->bone].parent != origParent)
		{
			MdlError( "unknown procedural bone parent remapping\n" );
		}

		if ( g_bonetable[pInterp->control].parent != origControlParent)
		{
			MdlError( "procedural bone \"%s\", parent remapping error, control parent was \"%s\", is now \"%s\"\n",
				pInterp->bonename, 
				pInterp->controlparentname,
				g_bonetable[g_bonetable[pInterp->control].parent].name );
		}

		// remap triggers and movements/rotations due to skeleton changes and realignment
		for (int k = 0; k < pInterp->numtriggers; k++)
		{
			int parent = g_bonetable[pInterp->control].parent;

			// triggers are the "control" bone relative to the control's parent bone
			if (parent != -1)
			{
				matrix3x4_t invControlParentRealign;
				MatrixInvert( g_bonetable[parent].srcRealign, invControlParentRealign );

				matrix3x4_t srcControlParentBoneToPose;
				ConcatTransforms( g_bonetable[parent].boneToPose, invControlParentRealign, srcControlParentBoneToPose );

				matrix3x4_t srcControlRelative;
				QuaternionMatrix( pInterp->trigger[k], srcControlRelative );

				matrix3x4_t srcControlBoneToPose;
				ConcatTransforms( srcControlParentBoneToPose, srcControlRelative, srcControlBoneToPose );

				matrix3x4_t destControlParentBoneToPose;
				ConcatTransforms( srcControlParentBoneToPose, g_bonetable[parent].srcRealign, destControlParentBoneToPose );

				matrix3x4_t destControlBoneToPose;
				ConcatTransforms( srcControlBoneToPose, g_bonetable[pInterp->control].srcRealign, destControlBoneToPose );

				matrix3x4_t invDestControlParentBoneToPose;
				MatrixInvert( destControlParentBoneToPose, invDestControlParentBoneToPose );

				matrix3x4_t destControlRelative;
				ConcatTransforms( invDestControlParentBoneToPose, destControlBoneToPose, destControlRelative );

				Vector tmp;
				MatrixAngles( destControlRelative, pInterp->trigger[k], tmp );

				/*
				Vector pos;
				RadianEuler angles;


				MatrixAngles( srcControlRelative, angles, pos );
				printf("srcControlRelative  : %7.2f %7.2f %7.2f\n", RAD2DEG( angles.x ), RAD2DEG( angles.y ), RAD2DEG( angles.z ) );

				MatrixAngles( destControlRelative, angles, pos );
				printf("destControlRelative : %7.2f %7.2f %7.2f\n", RAD2DEG( angles.x ), RAD2DEG( angles.y ), RAD2DEG( angles.z ) );

				printf("\n");
				*/
			}

			// movements are relative to the bone's parent
			parent = g_bonetable[pInterp->bone].parent;
			if (parent != -1)
			{
				//printf("procedural bone \"%s\"\n", pInterp->bonename );
				//printf("pre  : %7.2f %7.2f %7.2f\n", pInterp->pos[k].x, pInterp->pos[k].y, pInterp->pos[k].z );
				// get local transform
				matrix3x4_t srcParentRelative;
				QuaternionMatrix( pInterp->quat[k], pInterp->pos[k] + pInterp->basepos, srcParentRelative );

				// get original boneToPose
				matrix3x4_t invSrcRealign;
				MatrixInvert( g_bonetable[parent].srcRealign, invSrcRealign );
				matrix3x4_t origParentBoneToPose;
				ConcatTransforms( g_bonetable[parent].boneToPose, invSrcRealign, origParentBoneToPose );

				// move bone adjustment into world position
				matrix3x4_t srcBoneToWorld;
				ConcatTransforms( origParentBoneToPose, srcParentRelative, srcBoneToWorld );

				// calculate local transform
				matrix3x4_t parentPoseToBone;
				MatrixInvert( g_bonetable[parent].boneToPose, parentPoseToBone );
				matrix3x4_t destBoneToWorld;
				ConcatTransforms( parentPoseToBone, srcBoneToWorld, destBoneToWorld );

				// save out the local transform
				MatrixAngles( destBoneToWorld, pInterp->quat[k], pInterp->pos[k] );

				pInterp->pos[k] += g_bonetable[pInterp->control].pos * pInterp->percentage;

				//printf("post : %7.2f %7.2f %7.2f\n", pInterp->pos[k].x, pInterp->pos[k].y, pInterp->pos[k].z );
			}

		}
	}

	// look for aimatbones
	for (j = 0; j < g_numaimatbones; j++)
	{
		s_aimatbone_t *pAimAtBone = &g_aimatbones[g_aimatbonemap[j]];

		int origParent =	findGlobalBoneXSI( pAimAtBone->parentname );

		if (origParent == -1)
		{
			MdlError( "<aimconstraint> bone \"%s\", can't find parent bone \"%s\"\n\n", pAimAtBone->bonename, pAimAtBone->parentname );
		}

		int origAim( -1 );

		for ( int ai( 0 ); ai < g_numattachments; ++ai )
		{
			if ( strcmp( g_attachment[ ai ].name, pAimAtBone->aimname ) == 0 )
			{
				origAim = ai;
				break;
			}
		}

		if (origAim == -1)
		{
			MdlError( "<aimconstraint> bone \"%s\", can't find aim bone \"%s\n\n", pAimAtBone->bonename, pAimAtBone->aimname );
		}
	}

	// Look at Twist bones
	for ( j = g_twistbones.Count() - 1; j >= 0; --j )
	{
		CTwistBone &twistBone = g_twistbones[j];
		const int nParent = findGlobalBoneXSI( twistBone.m_szParentBoneName );
		if ( nParent < 0 )
		{
			MdlError( "<twistbone> Can't find parent bone \"%s\"\n", twistBone.m_szParentBoneName );
		}

		const int nChild = findGlobalBoneXSI( twistBone.m_szChildBoneName );
		if ( nChild  < 0 )
		{
			MdlError( "<twistbone> Can't find child bone \"%s\"\n", twistBone.m_szChildBoneName );
		}

		QuaternionAligned qParentSrcRealign;
		MatrixQuaternion( g_bonetable[nParent].srcRealign, qParentSrcRealign );

		QuaternionAligned qParentSrcRealignInv;
		QuaternionInvert( qParentSrcRealign, qParentSrcRealignInv );

		Vector vTmp;
		QuaternionAligned qTmp;

		if ( twistBone.m_bInverse )
		{
			RealignBoneQuaternion( twistBone.m_qBaseRotation, nParent, twistBone.m_qBaseRotation );

			VectorRotate( twistBone.m_vUpVector, qParentSrcRealignInv, vTmp );
			twistBone.m_vUpVector = vTmp;
		}
		else
		{
			QuaternionAligned qChildSrcRealign;
			MatrixQuaternion( g_bonetable[nChild].srcRealign, qChildSrcRealign );

			QuaternionAligned qChildSrcRealignInv;
			QuaternionInvert( qChildSrcRealign, qChildSrcRealignInv );

			RealignBoneQuaternion( twistBone.m_qBaseRotation, nChild, twistBone.m_qBaseRotation );

			VectorRotate( twistBone.m_vUpVector, qChildSrcRealignInv, vTmp );
			twistBone.m_vUpVector = vTmp;
		}

		for ( int k = twistBone.m_twistBoneTargets.Count() - 1; k >= 0; --k )
		{
			s_constraintbonetarget_t &twistBoneTarget = twistBone.m_twistBoneTargets[k];
			const int nBoneIndex = findGlobalBoneXSI( twistBoneTarget.m_szBoneName );
			if ( nBoneIndex < 0 )
			{
				MdlError( "<twistbone> Can't find target bone \"%s\"\n", twistBoneTarget.m_szBoneName );
			}

			VectorRotate( twistBoneTarget.m_vOffset, qParentSrcRealignInv, vTmp );
			twistBoneTarget.m_vOffset = vTmp;

			RealignBoneQuaternion( twistBoneTarget.m_qOffset, nBoneIndex, twistBoneTarget.m_qOffset );
		}
	}

	// Handle constraint bones
	RemapConstraintBones();
}


//-----------------------------------------------------------------------------
// Purpose: propogate procedural bone usage up its chain
//-----------------------------------------------------------------------------
void MarkProceduralBoneChain()
{
	int j;
	int	k;
	int	fBoneFlags;

	// look for QuatInterp bone definitions
	for (j = 0; j < g_numquatinterpbones; j++)
	{
		s_quatinterpbone_t *pInterp = &g_quatinterpbones[g_quatinterpbonemap[j]];
	
		fBoneFlags = g_bonetable[pInterp->bone].flags & BONE_USED_MASK;

		// propogate the procedural bone usage up its hierarchy
		k = pInterp->control;
		while (k != -1)
		{
			g_bonetable[k].flags |= fBoneFlags;
			k = g_bonetable[k].parent;
		}

		// propogate the procedural bone usage up its hierarchy
		k = pInterp->bone;
		while (k != -1)
		{
			g_bonetable[k].flags |= fBoneFlags;
			k = g_bonetable[k].parent;
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: go through all source files and link local bone indices and global bonetable indicies
//-----------------------------------------------------------------------------
static int MapSourcesToGlobalBonetable( )
{
	int i, j, k;
	int	iError = 0;

	// map each source bone list to master list
	for (i = 0; i < g_numsources; i++)
	{
		s_source_t *pSource = g_source[i];

		memset( pSource->boneLocalToGlobal, 0xFF, sizeof(pSource->boneLocalToGlobal) );
		memset( pSource->boneGlobalToLocal, 0xFF, sizeof(pSource->boneGlobalToLocal) );

		for ( j = 0; j < pSource->numbones; j++ )
		{
			k = findGlobalBone( pSource->localBone[j].name );
			if ( k >= 0 )
			{
				pSource->boneLocalToGlobal[j] = k;
				pSource->boneGlobalToLocal[k] = j;
				continue;
			}

			int m = pSource->localBone[j].parent;
			while ( m != -1 && ( k = findGlobalBone( pSource->localBone[m].name ) ) == -1 )
			{
				m = pSource->localBone[m].parent;
			}
			if (k == -1)
			{
				/*
				if (!g_quiet)
				{
					printf("unable to find connection for collapsed bone \"%s\" \n", pSource->localBone[j].name );
				}
				*/
				k = 0;
			}
			pSource->boneLocalToGlobal[j] = k;
		}
	}
	return iError;
}



//-----------------------------------------------------------------------------
// Purpose: go through bone and find any that arent aligned on the X axis
//-----------------------------------------------------------------------------
void RealignBones( )
{
	int k;

	int childbone[MAXSTUDIOBONES];
	for (k = 0; k < g_numbones; k++)
	{
		childbone[k] = -1;
	}

	// force bones with IK rules to realign themselves
	for (int i = 0; i < g_numikchains; i++)
	{
		k = g_ikchain[i].link[0].bone;
		if (childbone[k] == -1 || childbone[k] == g_ikchain[i].link[1].bone)
		{
			childbone[k] = g_ikchain[i].link[1].bone;
		}
		else
		{
			MdlError("Trying to realign bone \"%s\" with two children \"%s\", \"%s\"\n", 
				g_bonetable[k].name, g_bonetable[childbone[k]].name, g_bonetable[g_ikchain[i].link[1].bone].name );
		}

		k = g_ikchain[i].link[1].bone;
		if (childbone[k] == -1 || childbone[k] == g_ikchain[i].link[2].bone)
		{
			childbone[k] = g_ikchain[i].link[2].bone;
		}
		else
		{
			MdlError("Trying to realign bone \"%s\" with two children \"%s\", \"%s\"\n", 
				g_bonetable[k].name, g_bonetable[childbone[k]].name, g_bonetable[g_ikchain[i].link[2].bone].name );
		}
	}

	if (g_realignbones)
	{
		int children[MAXSTUDIOBONES];

		// count children
		for (k = 0; k < g_numbones; k++)
		{
			children[k] = 0;
		}
		for (k = 0; k < g_numbones; k++)
		{
			if (g_bonetable[k].parent != -1)
			{
				children[g_bonetable[k].parent]++;
			}
		}

		// if my parent bone only has one child, then tell it to align to me
		for (k = 0; k < g_numbones; k++)
		{
			if (g_bonetable[k].parent != -1 && children[g_bonetable[k].parent] == 1)
			{
				childbone[g_bonetable[k].parent] = k;
			}
		}
	}

	matrix3x4_t boneToPose[MAXSTUDIOBONES];

	for (k = 0; k < g_numbones; k++)
	{
		MatrixCopy( g_bonetable[k].boneToPose, boneToPose[k] );
	}

	// look for bones that aren't on a primary X axis
	for (k = 0; k < g_numbones; k++)
	{
		// printf("%s  %.4f %.4f %.4f  (%d)\n", g_bonetable[k].name, g_bonetable[k].pos.x, g_bonetable[k].pos.y, g_bonetable[k].pos.z, children[k] );
		if (!g_bonetable[k].bPreAligned && childbone[k] != -1)
		{
			float d = g_bonetable[childbone[k]].pos.Length();

			// check to see that it's on positive X
			if (d - g_bonetable[childbone[k]].pos.x > 0.01)
			{
				Vector v2;
				Vector v3;
				// printf("%s:%s  %.4f %.4f %.4f\n", g_bonetable[k].name, g_bonetable[childbone[k]].name, g_bonetable[childbone[k]].pos.x, g_bonetable[childbone[k]].pos.y, g_bonetable[childbone[k]].pos.z );

				Vector forward, left, up;

				// calc X axis
				MatrixGetColumn( g_bonetable[childbone[k]].boneToPose, 3, v2 );
				MatrixGetColumn( g_bonetable[k].boneToPose, 3, v3 );
				forward = v2 - v3;
				VectorNormalize( forward );

				// try to align to existing bone/boundingbox by finding most perpendicular 
				// existing axis and aligning the new Z axis to it.
				Vector forward2, left2, up2;
				MatrixGetColumn( boneToPose[k], 0, forward2 );
				MatrixGetColumn( boneToPose[k], 1, left2 );
				MatrixGetColumn( boneToPose[k], 2, up2 );
				float d1 = fabs(DotProduct( forward, forward2 ));
				float d2 = fabs(DotProduct( forward, left2 ));
				float d3 = fabs(DotProduct( forward, up2 ));
				if (d1 <= d2 && d1 <= d3)
				{
					up = CrossProduct( forward, forward2 );
					VectorNormalize( up );
				}
				else if (d2 <= d1 && d2 <= d3)
				{
					up = CrossProduct( forward, left2 );
					VectorNormalize( up );
				}
				else
				{
					up = CrossProduct( forward, up2 );
					VectorNormalize( up );
				}
				left = CrossProduct( up, forward );

				// setup matrix
				MatrixSetColumn( forward, 0, boneToPose[k] );
				MatrixSetColumn( left, 1, boneToPose[k] );
				MatrixSetColumn( up, 2, boneToPose[k] );

				// check orthonormality of matrix
				d =   fabs( DotProduct( forward, left ) ) 
					+ fabs( DotProduct( left, up ) ) 
					+ fabs( DotProduct( up, forward ) )
					+ fabs( DotProduct( boneToPose[k][0], boneToPose[k][1] ) )
					+ fabs( DotProduct( boneToPose[k][1], boneToPose[k][2] ) )
					+ fabs( DotProduct( boneToPose[k][2], boneToPose[k][0] ) );

				if (d > 0.0001)
				{
					MdlError( "error with realigning bone %s\n", g_bonetable[k].name );
				}

				// printf("%f %f %f\n", DotProduct( boneToPose[k][0], boneToPose[k][1] ), DotProduct( boneToPose[k][1], boneToPose[k][2] ), DotProduct( boneToPose[k][2], boneToPose[k][0] ) );

				// printf("%f %f %f\n", DotProduct( forward, left ), DotProduct( left, up ), DotProduct( up, forward ) );

				// VectorMatrix( forward, boneToPose[k] );

				MatrixSetColumn( v3, 3, boneToPose[k] );
			}
		}
	}

	for (int i = 0; i < g_numforcedrealign; i++)
	{
		k = findGlobalBone( g_forcedrealign[i].name );
		if (k == -1)
		{
			MdlWarning( "unknown bone %s in $forcedrealign\n", g_forcedrealign[i].name );
			continue;
		}

		matrix3x4_t local;
		matrix3x4_t tmp;

		QuaternionMatrix( g_forcedrealign[i].qrot, local );
		ConcatTransforms( boneToPose[k], local, tmp );
		MatrixCopy( tmp, boneToPose[k] );
	}

	// build realignment transforms
	for (k = 0; k < g_numbones; k++)
	{
		if (!g_bonetable[k].bPreAligned)
		{
			matrix3x4_t poseToBone;

			MatrixInvert( g_bonetable[k].boneToPose, poseToBone );
			ConcatTransforms( poseToBone, boneToPose[k], g_bonetable[k].srcRealign );

			MatrixCopy( boneToPose[k], g_bonetable[k].boneToPose );
		}
	}

	// printf("\n");

	// rebuild default angles, position, etc.
	for (k = 0; k < g_numbones; k++)
	{
		if (!g_bonetable[k].bPreAligned)
		{
			matrix3x4_t bonematrix;
			if (g_bonetable[k].parent == -1)
			{
				MatrixCopy( g_bonetable[k].boneToPose, bonematrix );
			}
			else
			{
				matrix3x4_t poseToBone;
				// convert my transform into parent relative space
				MatrixInvert( g_bonetable[g_bonetable[k].parent].boneToPose, poseToBone );
				ConcatTransforms( poseToBone, g_bonetable[k].boneToPose, bonematrix );
			}

			MatrixQuaternion( bonematrix, g_bonetable[k].qrot, g_bonetable[k].pos );
		}
	}

	// exit(0);

	// printf("\n");

	// build reference pose
	for (k = 0; k < g_numbones; k++)
	{
		matrix3x4_t bonematrix;
		QuaternionMatrix( g_bonetable[k].qrot, g_bonetable[k].pos, bonematrix );
		// MatrixCopy( g_bonetable[k].rawLocal, bonematrix );
		if (g_bonetable[k].parent == -1)
		{
			MatrixCopy( bonematrix, g_bonetable[k].boneToPose );
		}
		else
		{
			ConcatTransforms (g_bonetable[g_bonetable[k].parent].boneToPose, bonematrix, g_bonetable[k].boneToPose);
		}
		/*
		Vector v1;
		MatrixGetColumn( g_bonetable[k].boneToPose, 3, v1 );
		printf("%s  %.4f %.4f %.4f\n", g_bonetable[k].name, v1.x, v1.y, v1.z );
		*/
	}
}

void CenterBonesOnVerts( void )
{
	Vector bmin[MAXSTUDIOBONES];
	Vector bmax[MAXSTUDIOBONES];

	int i, j, k, n;

	for (k = 0; k < g_numbones; k++)
	{
		bmin[k] = Vector( 1, 1, 1 ) * 99999999.0;
		bmax[k] = Vector( 1, 1, 1 ) * -99999999.0;
	}

	// find domain of all the vertices
	for (i = 0; i < g_numsources; i++)
	{
		s_source_t *pSource = g_source[i];
		if ( !pSource->vertex )
			continue;

		s_sourceanim_t *pSourceAnim = FindSourceAnim( pSource, "BindPose" );
		if ( !pSourceAnim )
		{
			pSourceAnim = &pSource->m_Animations[0];
		}
		
		pSource->m_GlobalVertices.AddMultipleToTail( pSource->numvertices );

		Vector	p;
		for (j = 0; j < pSource->numvertices; j++)
		{
			for (n = 0; n < pSource->m_GlobalVertices[j].boneweight.numbones; n++)
			{
				k = pSource->m_GlobalVertices[j].boneweight.bone[n];
				p = pSource->m_GlobalVertices[j].position;

				bmin[k] = bmin[k].Min( p );
				bmax[k] = bmax[k].Max( p );
			}
		}
	}

	// copy min/maxs up to parent
	for (k = g_numbones - 1; k >= 0; k--)
	{
		if (bmin[k].x > bmax[k].x)
		{
			for (j = k + 1; j < g_numbones; j++)
			{
				if (g_bonetable[j].parent == k)
				{
					bmin[k] = bmin[k].Min( bmin[j] );
					bmax[k] = bmax[k].Max( bmax[j] );
				}
			}
		}
	}

	for (k = 0; k < g_numbones; k++)
	{
		if (bmin[k].x <= bmax[k].x)
		{
			Vector center = (bmin[k] + bmax[k]) * 0.5;
	
			// printf("%d %.1f %.1f %.1f\n", k, center.x, center.y, center.z );
			matrix3x4_t updateCenter;
			MatrixCopy( g_bonetable[k].boneToPose, updateCenter );
			PositionMatrix( center, updateCenter );

			matrix3x4_t invPoseToBone;
			MatrixInvert( g_bonetable[k].boneToPose, invPoseToBone );
			ConcatTransforms( invPoseToBone, updateCenter, g_bonetable[k].srcRealign );

			MatrixCopy( updateCenter, g_bonetable[k].boneToPose );
		}
	}
	
	// rebuild default angles, position, etc.
	for (k = 0; k < g_numbones; k++)
	{
		if (!g_bonetable[k].bPreAligned)
		{
			matrix3x4_t bonematrix;
			if (g_bonetable[k].parent == -1)
			{
				MatrixCopy( g_bonetable[k].boneToPose, bonematrix );
			}
			else
			{
				matrix3x4_t poseToBone;
				// convert my transform into parent relative space
				MatrixInvert( g_bonetable[g_bonetable[k].parent].boneToPose, poseToBone );
				ConcatTransforms( poseToBone, g_bonetable[k].boneToPose, bonematrix );
			}

			MatrixQuaternion( bonematrix, g_bonetable[k].qrot, g_bonetable[k].pos );
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: Find all attachments that have matching names
//          Remove those that are truly duplicates
//			Leave ones that aren't duplicates but warn about them
//-----------------------------------------------------------------------------
void RemoveDuplicateAttachments()
{
	for ( int i = 0; i < g_numattachments; ++i )
	{
		const s_attachment_t &iAtt = g_attachment[ i ];
		for ( int j = g_numattachments - 1; j > i; --j )
		{
			const s_attachment_t &jAtt = g_attachment[ j ];
			if ( Q_strcmp( iAtt.name, jAtt.name ) )
				continue;	// Not the same name

			if ( Q_stricmp( iAtt.bonename, jAtt.bonename ) ||
				iAtt.bone != jAtt.bone ||
				iAtt.type != jAtt.type ||
				iAtt.flags != jAtt.flags ||
				Q_memcmp( iAtt.local.Base(), jAtt.local.Base(), sizeof( matrix3x4_t ) ) )
			{
				RadianEuler iEuler, jEuler;
				Vector iPos, jPos;
				MatrixAngles( iAtt.local, iEuler, iPos );
				MatrixAngles( jAtt.local, jEuler, jPos );
				MdlWarning(
					"Attachments with the same name but different parameters found\n"
					"  %s: ParentBone: %s Type: %d Flags: 0x%08x P: %6.2f %6.2f %6.2f R: %6.2f %6.2f %6.2f\n"
					"  %s: ParentBone: %s Type: %d Flags: 0x%08x P: %6.2f %6.2f %6.2f R: %6.2f %6.2f %6.2f\n",
					iAtt.name, iAtt.bonename, iAtt.type, iAtt.flags,
					iPos.x, iPos.y, iPos.z, RAD2DEG( iEuler.x ), RAD2DEG( iEuler.y ), RAD2DEG( iEuler.z ),
					jAtt.name, jAtt.bonename, jAtt.type, jAtt.flags,
					jPos.x, jPos.y, jPos.z, RAD2DEG( jEuler.x ), RAD2DEG( jEuler.y ), RAD2DEG( jEuler.z ) );
				continue;
			}

			// Delete attachment j by shifting j+1 to the end down overtop of j

			Q_memcpy( &( g_attachment[ j ] ), &( g_attachment[ j + 1 ] ), ( g_numattachments - j - 1 ) * sizeof( s_attachment_t ) );
			--g_numattachments;
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: find all the different bones used in all the source files and map everything
//			to a common bonetable.
//-----------------------------------------------------------------------------
void RemapBones( )
{
	int	iError = 0;

	if ( g_staticprop )
	{
		MakeStaticProp( );
	}
	else if ( g_centerstaticprop )
	{
		MdlWarning("Ignoring option $autocenter.  Only supported on $staticprop models!!!\n" );
	}

	TagUsedBones( );

	RenameBones( );

	iError = BuildGlobalBonetable( );

	BuildGlobalBoneToPose( );

	EnforceHierarchy( );

	{
		int k, n;
		for ( k = 0; k < g_numbones; k++ )
		{
			// tag parent bones as being in the same way as their children
			n = g_bonetable[k].parent;
			while (n != -1)
			{
				g_bonetable[n].flags |= g_bonetable[k].flags;
				n = g_bonetable[n].parent;
			}
		}
	}

	if ( g_collapse_bones || g_numimportbones )
	{
		CollapseBones( );
	}


	if ( g_numbones >= MAXSTUDIOBONES )
	{
		// export bones
		if (g_definebones)
		{
			DumpDefineBones();
		}
	
		MdlError( "Too many bones used in model, used %d, max %d\n", g_numbones, MAXSTUDIOBONES );
	}

	/*
	for (i = 0; i < g_numbones; i++)
	{
		printf("%2d %s %d\n", i, g_bonetable[i].name, g_bonetable[i].parent );
	}
	*/

	RebuildLocalPose( );

	TagProceduralBones( );

	if ( iError && !(ignore_warnings) )
	{
		MdlError( "Exiting due to errors\n" );
	}
	MapSourcesToGlobalBonetable( );

	if ( iError && !(ignore_warnings) )
	{
		MdlError( "Exiting due to errors\n" );
	}

	// Map the bone names to global bone indices for all BoneFlexDrivers
	MapFlexDriveBonesToGlobalBoneTable();
}



//-----------------------------------------------------------------------------
// Purpose: calculate the bone to world transforms for a processed animation
//-----------------------------------------------------------------------------
void CalcBoneTransforms( s_animation_t *panimation, int frame, matrix3x4_t* pBoneToWorld )
{
	CalcBoneTransforms( panimation, g_panimation[0], frame, pBoneToWorld );
}


void CalcBoneTransforms( s_animation_t *panimation, s_animation_t *pbaseanimation, int frame, matrix3x4_t* pBoneToWorld )
{
	if ((panimation->flags & STUDIO_LOOPING) && panimation->numframes > 1)
	{
		while (frame >= (panimation->numframes - 1))
		{
			frame = frame - (panimation->numframes - 1);
		}
	}
	if (frame < 0 || frame >= panimation->numframes)
	{
		MdlError("requested out of range frame on animation \"%s\" : %d (%d)\n", panimation->name, frame, panimation->numframes );
	}

	for (int k = 0; k < g_numbones; k++)
	{
		Vector angle;
		matrix3x4_t bonematrix;

		if (!(panimation->flags & STUDIO_DELTA))
		{
			QuaternionMatrix( panimation->sanim[frame][k].qrot, panimation->sanim[frame][k].pos, bonematrix );
		}
		else if (pbaseanimation)
		{
			Quaternion q1, q2, q3;
			Vector p3;

			//AngleQuaternion( g_bonetable[k].rot, q1 );
			QuaternionCopy( pbaseanimation->sanim[0][k].qrot, q1 );
			QuaternionCopy( panimation->sanim[frame][k].qrot, q2 );

			float s = panimation->weight[k];

			QuaternionMA( q1, s, q2, q3 );
			//p3 = g_bonetable[k].pos + s * panimation->sanim[frame][k].pos;
			p3 = pbaseanimation->sanim[0][k].pos + s * panimation->sanim[frame][k].pos;

			QuaternionMatrix(q3, p3, bonematrix );
		}
		else
		{
			Quaternion q1, q2, q3;
			Vector p3;

			QuaternionCopy( g_bonetable[k].qrot, q1 );
			QuaternionCopy( panimation->sanim[frame][k].qrot, q2 );

			float s = panimation->weight[k];

			QuaternionMA( q1, s, q2, q3 );
			//p3 = g_bonetable[k].pos + s * panimation->sanim[frame][k].pos;
			p3 = pbaseanimation->sanim[0][k].pos + s * g_bonetable[k].pos;

			QuaternionMatrix( q3, p3, bonematrix );
		}

		if (g_bonetable[k].parent == -1)
		{
			MatrixCopy( bonematrix, pBoneToWorld[k] );
		}
		else
		{
			ConcatTransforms (pBoneToWorld[g_bonetable[k].parent], bonematrix, pBoneToWorld[k]);
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: calculate the bone to world transforms for a processed animation
//-----------------------------------------------------------------------------

void CalcBoneTransformsCycle( s_animation_t *panimation, s_animation_t *pbaseanimation, float flCycle, matrix3x4_t* pBoneToWorld )
{
	float fFrame = flCycle * (panimation->numframes - 1);
	int iFrame = (int)fFrame;
	float s = (fFrame - iFrame);

	int iFrame1 = iFrame % (panimation->numframes - 1);
	int iFrame2 = (iFrame + 1) % (panimation->numframes - 1);

	for (int k = 0; k < g_numbones; k++)
	{
		Quaternion q1, q2, q3;
		Vector p3;
		matrix3x4_t bonematrix;

		// if (!(panimation->flags & STUDIO_DELTA))
		{
			QuaternionCopy( panimation->sanim[iFrame1][k].qrot, q1 );
			QuaternionCopy( panimation->sanim[iFrame2][k].qrot, q2 );
			QuaternionSlerp( q1, q2, s, q3 );

			VectorLerp( panimation->sanim[iFrame1][k].pos, panimation->sanim[iFrame2][k].pos, s, p3 );

			AngleMatrix( RadianEuler( q3 ), p3, bonematrix );
		}
		/* 
		else
		{
			Vector p3;

			//AngleQuaternion( g_bonetable[k].rot, q1 );
			AngleQuaternion( pbaseanimation->sanim[0][k].rot, q1 );
			AngleQuaternion( panimation->sanim[frame][k].rot, q2 );

			float s = panimation->weight[k];

			QuaternionMA( q1, s, q2, q3 );
			//p3 = g_bonetable[k].pos + s * panimation->sanim[frame][k].pos;
			p3 = pbaseanimation->sanim[0][k].pos + s * panimation->sanim[frame][k].pos;

			AngleMatrix( q3, p3, bonematrix );
		}
		*/

		if (g_bonetable[k].parent == -1)
		{
			MatrixCopy( bonematrix, pBoneToWorld[k] );
		}
		else
		{
			ConcatTransforms (pBoneToWorld[g_bonetable[k].parent], bonematrix, pBoneToWorld[k]);
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: calculate the bone to world transforms for a processed sequence
//-----------------------------------------------------------------------------


void SlerpBones( 
	Quaternion q1[MAXSTUDIOBONES], 
	Vector pos1[MAXSTUDIOBONES], 
	int sequence, 
	const Quaternion q2[MAXSTUDIOBONES], 
	const Vector pos2[MAXSTUDIOBONES], 
	float s )
{
	int			i;
	Quaternion		q3, q4;
	float		s1, s2;

	s_sequence_t *pseqdesc = &g_sequence[sequence];

	if (s <= 0.0f) 
	{
		return;
	}
	else if (s > 1.0f)
	{
		s = 1.0f;		
	}

	if (pseqdesc->flags & STUDIO_DELTA)
	{
		for (i = 0; i < g_numbones; i++)
		{

			s2 = s * pseqdesc->weight[i];	// blend in based on this bones weight
			if (s2 > 0.0)
			{
				if (pseqdesc->flags & STUDIO_POST)
				{
					QuaternionMA( q1[i], s2, q2[i], q1[i] );

					// FIXME: are these correct?
					pos1[i][0] = pos1[i][0] + pos2[i][0] * s2;
					pos1[i][1] = pos1[i][1] + pos2[i][1] * s2;
					pos1[i][2] = pos1[i][2] + pos2[i][2] * s2;
				}
				else
				{
					QuaternionSM( s2, q2[i], q1[i], q1[i] );

					// FIXME: are these correct?
					pos1[i][0] = pos1[i][0] + pos2[i][0] * s2;
					pos1[i][1] = pos1[i][1] + pos2[i][1] * s2;
					pos1[i][2] = pos1[i][2] + pos2[i][2] * s2;
				}
			}
		}
	}
	else
	{
		for (i = 0; i <g_numbones; i++)
		{
			s2 = s * pseqdesc->weight[i];	// blend in based on this animations weights
			if (s2 > 0.0)
			{
				s1 = 1.0 - s2;

				if (g_bonetable[i].flags & BONE_FIXED_ALIGNMENT)
				{
					QuaternionSlerpNoAlign( q2[i], q1[i], s1, q3 );
				}
				else
				{
					QuaternionSlerp( q2[i], q1[i], s1, q3 );
				}
				q1[i][0] = q3[0];
				q1[i][1] = q3[1];
				q1[i][2] = q3[2];
				q1[i][3] = q3[3];
				pos1[i][0] = pos1[i][0] * s1 + pos2[i][0] * s2;
				pos1[i][1] = pos1[i][1] * s1 + pos2[i][1] * s2;
				pos1[i][2] = pos1[i][2] * s1 + pos2[i][2] * s2;
			}
		}
	}
}


void CalcPoseSingle( Vector pos[], Quaternion q[], int sequence, float frame )
{
	s_sequence_t *pseqdesc = &g_sequence[sequence];

	s_animation_t *panim = pseqdesc->panim[0][0];

	// FIXME: is this modulo correct?
	int iframe = ((int)frame) % panim->numframes;

	for (int k = 0; k < g_numbones; k++)
	{
		// FIXME: this isn't doing a fractional frame
			// FIXME 2025: After a.. 8+ years ? it still needs to be fixed.
		QuaternionCopy( panim->sanim[iframe][k].qrot, q[k] );
		pos[k] = panim->sanim[iframe][k].pos;
	}
}

void AccumulateSeqLayers( Vector pos[], Quaternion q[], int sequence, float frame, float flWeight );

void AccumulatePose( Vector pos[], Quaternion q[], int sequence, float frame, float flWeight )
{
	Vector		pos2[MAXSTUDIOBONES];
	Quaternion	q2[MAXSTUDIOBONES];

	// printf("accumulate %s : %.1f\n", g_sequence[sequence].name, frame );

	CalcPoseSingle( pos2, q2, sequence, frame );

	SlerpBones( q, pos, sequence, q2, pos2, flWeight );

	AccumulateSeqLayers( pos, q, sequence, frame, flWeight );
}

void AccumulateSeqLayers( Vector pos[], Quaternion q[], int sequence, float frame, float flWeight )
{
	s_sequence_t *pseqdesc = &g_sequence[sequence];

	for (int i = 0; i < pseqdesc->numautolayers; i++)
	{
		s_autolayer_t *pLayer = &pseqdesc->autolayer[i];

		float layerFrame = frame;
		float layerWeight = flWeight;

		if (pLayer->start != pLayer->end)
		{
			float s = 1.0;
			float index;

			if (!(pLayer->flags & STUDIO_AL_POSE))
			{
				index = frame;
			}
			else
			{
				int iPose = pLayer->pose;
				if (iPose != -1)
				{
					index = 0; // undefined?
				}
				else
				{
					index = 0;
				}
			}

			if (index < pLayer->start)
				continue;
			if (index >= pLayer->end)
				continue;

			if (index < pLayer->peak && pLayer->start != pLayer->peak)
			{
				s = (index - pLayer->start) / (pLayer->peak - pLayer->start);
			}
			else if (index > pLayer->tail && pLayer->end != pLayer->tail)
			{
				s = (pLayer->end - index) / (pLayer->end - pLayer->tail);
			}

			if (pLayer->flags & STUDIO_AL_SPLINE)
			{
				s = 3 * s * s - 2 * s * s * s;
			}

			if ((pLayer->flags & STUDIO_AL_XFADE) && (frame > pLayer->tail))
			{
				layerWeight = ( s * flWeight ) / ( 1 - flWeight + s * flWeight );
			}
			else if (pLayer->flags & STUDIO_AL_NOBLEND)
			{
				layerWeight = s;
			}
			else
			{
				layerWeight = flWeight * s;
			}

			if (!(pLayer->flags & STUDIO_AL_POSE))
			{
				layerFrame = ((frame - pLayer->start) / (pLayer->end - pLayer->start)) * (g_sequence[pLayer->sequence].panim[0][0]->numframes - 1);
			}
			else
			{
				layerFrame = (frame / g_sequence[sequence].panim[0][0]->numframes - 1) * (g_sequence[pLayer->sequence].panim[0][0]->numframes - 1);
			}
		}

		AccumulatePose( pos, q, pLayer->sequence, layerFrame, layerWeight );
	}
}


void CalcSeqTransforms( int sequence, int frame, matrix3x4_t* pBoneToWorld )
{
	int k;
	Vector		pos[MAXSTUDIOBONES];
	Quaternion	q[MAXSTUDIOBONES];

	// CalcPoseSingle( pos, q, 0, 0 );
	/*
	for (k = 0; k < g_numbones; k++)
	{
		//AngleQuaternion( g_bonetable[k].rot, q[k] );
		//pos[k] = g_bonetable[k].pos;
		AngleQuaternion( g_bonetable[k].rot, q[k] );
		pos[k] = g_bonetable[k].pos;
	}
	*/

	for (k = 0; k < g_numbones; k++)
	{
		//AngleQuaternion( g_bonetable[k].rot, q[k] );
		//pos[k] = g_bonetable[k].pos;
		QuaternionCopy( g_bonetable[k].qrot, q[k] );
		pos[k] = g_bonetable[k].pos;
	}


	AccumulatePose( pos, q, sequence, frame, 1.0 );

	for (k = 0; k < g_numbones; k++)
	{
		matrix3x4_t bonematrix;

		QuaternionMatrix( q[k], pos[k], bonematrix );

		if (g_bonetable[k].parent == -1)
		{
			MatrixCopy( bonematrix, pBoneToWorld[k] );
		}
		else
		{
			ConcatTransforms (pBoneToWorld[g_bonetable[k].parent], bonematrix, pBoneToWorld[k]);
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------

void CalcBonePos( s_animation_t *panimation, int frame, int bone, Vector &pos )
{
	matrix3x4_t boneToWorld[MAXSTUDIOSRCBONES];	// bone transformation matrix

	CalcBoneTransforms( panimation, frame, boneToWorld );

	pos.x = boneToWorld[bone][0][3];
	pos.y = boneToWorld[bone][1][3];
	pos.z = boneToWorld[bone][2][3];
}


#define SMALL_FLOAT 1e-12

// NOTE: This routine was taken (and modified) from NVidia's BlinnReflection demo
// Creates basis vectors, based on a vertex and index list.
// See the NVidia white paper 'GDC2K PerPixel Lighting' for a description
// of how this computation works
static void CalcTriangleTangentSpace( s_source_t *pSrc, int v1, int v2, int v3, 
									  Vector &sVect, Vector &tVect )
{
/*
	static bool firstTime = true;
	static FILE *fp = NULL;
	if( firstTime )
	{
		firstTime = false;
		fp = fopen( "crap.out", "w" );
	}
*/
    
	Vector2D t0( pSrc->vertex[v1].texcoord[0][0], pSrc->vertex[v1].texcoord[0][1] );
	Vector2D t1( pSrc->vertex[v2].texcoord[0][0], pSrc->vertex[v2].texcoord[0][1] );
	Vector2D t2( pSrc->vertex[v3].texcoord[0][0], pSrc->vertex[v3].texcoord[0][1] );
	Vector p0( pSrc->vertex[v1].position[0], pSrc->vertex[v1].position[1], pSrc->vertex[v1].position[2] );
	Vector p1( pSrc->vertex[v2].position[0], pSrc->vertex[v2].position[1], pSrc->vertex[v2].position[2] );
	Vector p2( pSrc->vertex[v3].position[0], pSrc->vertex[v3].position[1], pSrc->vertex[v3].position[2] );
	CalcTriangleTangentSpace( p0, p1, p2, t0, t1, t2, sVect, tVect );

/*
	// Calculate flat normal
	Vector flatNormal;
	edge01 = p1 - p0;
	edge02 = p2 - p0;
	CrossProduct( edge02, edge01, flatNormal );
	VectorNormalize( flatNormal );
	
	// Get the average position
	Vector avgPos = ( p0 + p1 + p2 ) / 3.0f;

	// Draw the svect
	Vector endS = avgPos + sVect * .2f;
	fprintf( fp, "2\n" );
	fprintf( fp, "%f %f %f 1.0 0.0 0.0\n", endS[0], endS[1], endS[2] );
	fprintf( fp, "%f %f %f 1.0 0.0 0.0\n", avgPos[0], avgPos[1], avgPos[2] );
	
	// Draw the tvect
	Vector endT = avgPos + tVect * .2f;
	fprintf( fp, "2\n" );
	fprintf( fp, "%f %f %f 0.0 1.0 0.0\n", endT[0], endT[1], endT[2] );
	fprintf( fp, "%f %f %f 0.0 1.0 0.0\n", avgPos[0], avgPos[1], avgPos[2] );
	
	// Draw the normal
	Vector endN = avgPos + flatNormal * .2f;
	fprintf( fp, "2\n" );
	fprintf( fp, "%f %f %f 0.0 0.0 1.0\n", endN[0], endN[1], endN[2] );
	fprintf( fp, "%f %f %f 0.0 0.0 1.0\n", avgPos[0], avgPos[1], avgPos[2] );
	
	// Draw the wireframe of the triangle in white.
	fprintf( fp, "2\n" );
	fprintf( fp, "%f %f %f 1.0 1.0 1.0\n", p0[0], p0[1], p0[2] );
	fprintf( fp, "%f %f %f 1.0 1.0 1.0\n", p1[0], p1[1], p1[2] );
	fprintf( fp, "2\n" );
	fprintf( fp, "%f %f %f 1.0 1.0 1.0\n", p1[0], p1[1], p1[2] );
	fprintf( fp, "%f %f %f 1.0 1.0 1.0\n", p2[0], p2[1], p2[2] );
	fprintf( fp, "2\n" );
	fprintf( fp, "%f %f %f 1.0 1.0 1.0\n", p2[0], p2[1], p2[2] );
	fprintf( fp, "%f %f %f 1.0 1.0 1.0\n", p0[0], p0[1], p0[2] );

	// Draw a slightly shrunken version of the geometry to hide surfaces
	Vector tmp0 = p0 - flatNormal * .1f;
	Vector tmp1 = p1 - flatNormal * .1f;
	Vector tmp2 = p2 - flatNormal * .1f;
	fprintf( fp, "3\n" );
	fprintf( fp, "%f %f %f 0.1 0.1 0.1\n", tmp0[0], tmp0[1], tmp0[2] );
	fprintf( fp, "%f %f %f 0.1 0.1 0.1\n", tmp1[0], tmp1[1], tmp1[2] );
	fprintf( fp, "%f %f %f 0.1 0.1 0.1\n", tmp2[0], tmp2[1], tmp2[2] );
		
	fflush( fp );
*/
}

typedef CUtlVector<int> CIntVector;

void CalcModelTangentSpaces( s_source_t *pSrc )
{
	// Build a map from vertex to a list of faces that share the vert
	int meshID;
	for( meshID = 0; meshID < pSrc->nummeshes; meshID++ )
	{
		s_mesh_t *pMesh = &pSrc->mesh[pSrc->meshindex[meshID]];
		CUtlVector<CIntVector> vertToFaceMap;
		vertToFaceMap.AddMultipleToTail( pMesh->numvertices );
		for( int faceID = 0; faceID < pMesh->numfaces; faceID++ )
		{
			s_face_t *pFace = &pSrc->face[faceID + pMesh->faceoffset];
			vertToFaceMap[pFace->a].AddToTail( faceID );
			vertToFaceMap[pFace->b].AddToTail( faceID );
			vertToFaceMap[pFace->c].AddToTail( faceID );
			if ( pFace->d != 0xFFFFFFFF )						// SubD Quad face
			{
				vertToFaceMap[pFace->d].AddToTail( faceID );
			}
		}

		// Calculate the tangent space for each face
		CUtlVector<Vector> faceSVect;
		CUtlVector<Vector> faceTVect;
		faceSVect.AddMultipleToTail( pMesh->numfaces );
		faceTVect.AddMultipleToTail( pMesh->numfaces );
		for( int faceID = 0; faceID < pMesh->numfaces; faceID++ )
		{
			s_face_t *pFace = &pSrc->face[faceID + pMesh->faceoffset];

			CalcTriangleTangentSpace( pSrc, 
				pMesh->vertexoffset + pFace->a, 
				pMesh->vertexoffset + pFace->b, 
				pMesh->vertexoffset + pFace->c, 
				faceSVect[faceID], faceTVect[faceID] );
		}	

		// Calculate an average tangent space for each vertex.
		for( int vertID = 0; vertID < pMesh->numvertices; vertID++ )
		{
			const Vector &normal = pSrc->vertex[vertID+pMesh->vertexoffset].normal;
			Vector4D &finalSVect = pSrc->vertex[vertID+pMesh->vertexoffset].tangentS;
			Vector sVect, tVect;

			sVect.Init( 0.0f, 0.0f, 0.0f );
			tVect.Init( 0.0f, 0.0f, 0.0f );
			for( int faceID = 0; faceID < vertToFaceMap[vertID].Count(); faceID++ )
			{
				sVect += faceSVect[vertToFaceMap[vertID][faceID]];
				tVect += faceTVect[vertToFaceMap[vertID][faceID]];
			}

			// In the case of zbrush, everything needs to be treated as smooth.
			if( g_bZBrush )
			{
				Vector vertPos1( pSrc->vertex[vertID].position[0], pSrc->vertex[vertID].position[1], pSrc->vertex[vertID].position[2] );
				for( int vertID2 = 0; vertID2 < pMesh->numvertices; vertID2++ )
				{
					if( vertID2 == vertID )
					{
						continue;
					}
					Vector vertPos2( pSrc->vertex[vertID2].position[0], pSrc->vertex[vertID2].position[1], pSrc->vertex[vertID2].position[2] );
					if( vertPos1 == vertPos2 )
					{
						for( int faceID = 0; faceID < vertToFaceMap[vertID2].Count(); faceID++ )
						{
							sVect += faceSVect[vertToFaceMap[vertID2][faceID]];
							tVect += faceTVect[vertToFaceMap[vertID2][faceID]];
						}
					}
				}
			}

			// Make an orthonormal system.
			// Need to check if we are left or right handed.
			Vector tmpVect;
			CrossProduct( sVect, tVect, tmpVect );
			bool leftHanded = DotProduct( tmpVect, normal ) < 0.0f;
			if( !leftHanded )
			{
				CrossProduct( normal, sVect, tVect );
				CrossProduct( tVect, normal, sVect );
				VectorNormalize( sVect );
				VectorNormalize( tVect );
				finalSVect[0] = sVect[0];
				finalSVect[1] = sVect[1];
				finalSVect[2] = sVect[2];
				finalSVect[3] = 1.0f;
			}
			else
			{
				CrossProduct( sVect, normal, tVect );
				CrossProduct( normal, tVect, sVect );
				VectorNormalize( sVect );
				VectorNormalize( tVect );
				finalSVect[0] = sVect[0];
				finalSVect[1] = sVect[1];
				finalSVect[2] = sVect[2];
				finalSVect[3] = -1.0f;
			}
		}
	}
}


//-----------------------------------------------------------------------------
// Generate a model vertex from a source vertex
//-----------------------------------------------------------------------------
static void InitRemappedVertex( s_source_t *pSource, matrix3x4_t *pDestBoneToWorld, const s_vertexinfo_t &srcVertex, s_vertexinfo_t &dstVertex )
{
	Vector tmp1, tmp2, vdest, ndest;

	memcpy( &dstVertex, &srcVertex, sizeof(s_vertexinfo_t) );
	dstVertex.boneweight.numbones = 0;

	vdest.Init();
	ndest.Init();

	int n;
	for ( n = 0; n < srcVertex.boneweight.numbones; n++ )
	{
		// src bone
		int q = srcVertex.boneweight.bone[n];

		// mapping to global bone
		int k = pSource->boneLocalToGlobal[q];
		if ( k == -1 )
		{
			VectorCopy( srcVertex.position, vdest );
			VectorCopy( srcVertex.normal, ndest );
			break;
			// printf("%s:%s (%d) missing global\n", psource->filename, psource->localBone[q].name, q );
		}

		// If the global bone is already in the list, then this vertex
		// contains influences from multiple local bones which have been collapsed 
		// into a single global bone
		int m;
		for ( m = 0; m < dstVertex.boneweight.numbones; m++ )
		{
			if ( k == dstVertex.boneweight.bone[m] )
			{
				// bone got collapsed out
				dstVertex.boneweight.weight[m] += srcVertex.boneweight.weight[n];
				break;
			}
		}
		if ( m == dstVertex.boneweight.numbones )
		{
			// add new bone
			dstVertex.boneweight.bone[m] = k;
			dstVertex.boneweight.weight[m] = srcVertex.boneweight.weight[n];
			dstVertex.boneweight.numbones++;
		}

		// convert vertex into original models' bone local space
		VectorITransform( srcVertex.position, pDestBoneToWorld[k], tmp1 );
		// convert that into global world space using stardard pose
		VectorTransform( tmp1, g_bonetable[k].boneToPose, tmp2 );
		// accumulate
		VectorMA( vdest, srcVertex.boneweight.weight[n], tmp2, vdest );

		// convert normal into original models' bone local space
		VectorIRotate( srcVertex.normal, pDestBoneToWorld[k], tmp1 );
		// convert that into global world space using stardard pose
		VectorRotate( tmp1, g_bonetable[k].boneToPose, tmp2 );
		// accumulate
		VectorMA( ndest, srcVertex.boneweight.weight[n], tmp2, ndest );
	}

	// printf("%d  %.2f %.2f %.2f\n", j, vdest.x, vdest.y, vdest.z );

	// save, normalize
	VectorCopy( vdest, dstVertex.position );
	VectorNormalize( ndest );
	VectorCopy( ndest, dstVertex.normal );

	// FIXME: Remapping will whack tangentS. Need to recompute tangents after remapping
}


//-----------------------------------------------------------------------------
// When read off disk, s_source_t contains bone indices local to the source
// we need to make the bone indices use the global bone list
//-----------------------------------------------------------------------------
void RemapVerticesToGlobalBones( )
{
	matrix3x4_t srcBoneToWorld[MAXSTUDIOSRCBONES];
	matrix3x4_t destBoneToWorld[MAXSTUDIOSRCBONES];

	s_vertexinfo_t vTmpSrc;
	s_vertexinfo_t vTmpDst;

	for (int i = 0; i < g_numsources; i++)
	{
		s_source_t *pSource = g_source[i];
		if ( !pSource->vertex )
			continue;

		s_sourceanim_t *pSourceAnim = FindSourceAnim( pSource, "BindPose" );
		if ( !pSourceAnim )
		{
			pSourceAnim = &pSource->m_Animations[0];
		}
		BuildRawTransforms( pSource, pSourceAnim->animationname, 0, srcBoneToWorld );
		TranslateAnimations( pSource, srcBoneToWorld, destBoneToWorld );
		
		pSource->m_GlobalVertices.AddMultipleToTail( pSource->numvertices );

		for ( int j = 0; j < pSource->numvertices; j++ )
		{
			InitRemappedVertex( pSource, destBoneToWorld, pSource->vertex[j], pSource->m_GlobalVertices[j] );
		}

		// Loop through all animations on this source and remap vertex animations
		for ( int nAnimIndex = 0; nAnimIndex < pSource->m_Animations.Count(); ++nAnimIndex )
		{
			s_sourceanim_t *pAnim = &pSource->m_Animations[ nAnimIndex ];

			// Only remap newStyleVertexAnimations
			if ( !pAnim->newStyleVertexAnimations )
				continue;

			for ( int nFrameIndex = 0; nFrameIndex < pAnim->numframes; ++nFrameIndex )
			{
				// Only process frames which have data
				const int nVertexCount = pAnim->numvanims[ nFrameIndex ];
				if ( nVertexCount <= 0 )
					continue;

				s_vertanim_t *pVertAnims = pAnim->vanim[ nFrameIndex ];
				for ( int nVertexIndex = 0; nVertexIndex < nVertexCount; ++nVertexIndex )
				{
					s_vertanim_t &vertAnim = pVertAnims[ nVertexIndex ];
					const s_vertexinfo_t &vertex = pSource->vertex[ vertAnim.vertex ];
					memcpy( &vTmpSrc, &vertex, sizeof( s_vertexinfo_t ) );

					VectorAdd( vertex.position, vertAnim.pos, vTmpSrc.position );
					VectorAdd( vertex.normal, vertAnim.normal, vTmpSrc.normal );

					InitRemappedVertex( pSource, destBoneToWorld, vTmpSrc, vTmpDst );

					const s_vertexinfo_t &globalVertex = pSource->m_GlobalVertices[ vertAnim.vertex ];

					VectorSubtract( vTmpDst.position, globalVertex.position, vertAnim.pos );
					VectorSubtract( vTmpDst.normal, globalVertex.normal, vertAnim.normal );
				}
			}
		}
	}
}


//-----------------------------------------------------------------------------
// Links bone controllers
//-----------------------------------------------------------------------------

static void FindAutolayers()
{
	int i;
	for (i = 0; i < g_sequence.Count(); i++)
	{
		int k;
		for (k = 0; k < g_sequence[i].numautolayers; k++)
		{
			int j;
			for ( j = 0; j < g_sequence.Count(); j++)
			{
				if (stricmp( g_sequence[i].autolayer[k].name, g_sequence[j].name) == 0)
				{
					g_sequence[i].autolayer[k].sequence = j;
					break;
				}
			}
			if (j == g_sequence.Count())
			{
				MdlError( "sequence \"%s\" cannot find autolayer sequence \"%s\"\n",
					g_sequence[i].name, g_sequence[i].autolayer[k].name );
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Links bone controllers
//-----------------------------------------------------------------------------

static void LinkBoneControllers()
{
	for (int i = 0; i < g_numbonecontrollers; i++)
	{
		int j = findGlobalBone( g_bonecontroller[i].name );
		if (j == -1)
		{
			MdlError("unknown g_bonecontroller link '%s'\n", g_bonecontroller[i].name );
		}
		g_bonecontroller[i].bone = j;
	}
}

//-----------------------------------------------------------------------------
// Links screen aligned bones
//-----------------------------------------------------------------------------

static void TagScreenAlignedBones()
{
	for (int i = 0; i < g_numscreenalignedbones; i++)
	{
		int j = findGlobalBone( g_screenalignedbone[i].name );
		if (j == -1)
		{
			MdlError("unknown g_screenalignedbone link '%s'\n", g_screenalignedbone[i].name );
		}

		g_bonetable[j].flags |= g_screenalignedbone[i].flags;
		printf("tagging bone: %s as screen aligned (index %i, flags:%x)\n", g_bonetable[j].name, j, g_bonetable[j].flags );
	}
}

//-----------------------------------------------------------------------------
// world aligned bones
//-----------------------------------------------------------------------------

static void TagWorldAlignedBones()
{
	for (int i = 0; i < g_numworldalignedbones; i++)
	{
		int j = findGlobalBone( g_worldalignedbone[i].name );
		if (j == -1)
		{
			MdlError("unknown g_worldalignedbone link '%s'\n", g_worldalignedbone[i].name );
		}

		g_bonetable[j].flags |= g_worldalignedbone[i].flags;
		printf("tagging bone: %s as world aligned (index %i, flags:%x)\n", g_bonetable[j].name, j, g_bonetable[j].flags );
	}
}

//-----------------------------------------------------------------------------
// Links attachments
//-----------------------------------------------------------------------------

static void LinkAttachments()
{
	int i, j, k;

	// attachments may be connected to bones that can be optimized out
	// so search through all the sources and move to a valid location

	matrix3x4_t boneToPose;
	matrix3x4_t world;
	matrix3x4_t poseToBone;

	for (i = 0; i < g_numattachments; i++)
	{
		bool found = false;
		// search through known bones
		for (k = 0; k < g_numbones; k++)
		{
			if ( !stricmp( g_attachment[i].bonename, g_bonetable[k].name ))
			{
				g_attachment[i].bone = k;
				MatrixCopy( g_bonetable[k].boneToPose, boneToPose );
				MatrixInvert( boneToPose, poseToBone );
				// printf("%s : %d\n", g_bonetable[k].name, k );
				found = true;
				break;
			}
		}

		if (!found)
		{
			// search all the loaded sources for the first occurance of the named bone
			for (j = 0; j < g_numsources && !found; j++)
			{
				for (k = 0; k < g_source[j]->numbones && !found; k++)
				{
					if ( !stricmp( g_attachment[i].bonename, g_source[j]->localBone[k].name ) )
					{
						MatrixCopy( g_source[j]->boneToPose[k], boneToPose );

						// check to make sure that this bone is actually referenced in the output model
						// if not, try parent bone until we find a referenced bone in this chain
						while (k != -1 && g_source[j]->boneGlobalToLocal[g_source[j]->boneLocalToGlobal[k]] != k)
						{
							k = g_source[j]->localBone[k].parent;
						}
						if (k == -1)
						{
							MdlError( "unable to find valid bone for attachment %s:%s\n", 
								g_attachment[i].name,
								g_attachment[i].bonename );
						}

						MatrixInvert( g_source[j]->boneToPose[k], poseToBone );
						g_attachment[i].bone = g_source[j]->boneLocalToGlobal[k];
						found = true;
					}
				}
			}
		}

		if (!found)
		{
			MdlError("unknown attachment link '%s'\n", g_attachment[i].bonename );
		}
		// printf("%s: %s / %s\n", g_attachment[i].name, g_attachment[i].bonename, g_bonetable[g_attachment[i].bone].name );

		if (g_attachment[i].type & IS_ABSOLUTE)
		{
			MatrixCopy( g_attachment[i].local, world );
		}
		else
		{
			ConcatTransforms( boneToPose, g_attachment[i].local, world );
		}

		ConcatTransforms( poseToBone, world, g_attachment[i].local );
	}

	RemoveDuplicateAttachments();

	// flag all bones used by attachments
	for (i = 0; i < g_numattachments; i++)
	{
		j = g_attachment[i].bone;
		while (j != -1)
		{
			g_bonetable[j].flags |= BONE_USED_BY_ATTACHMENT;
			j = g_bonetable[j].parent;
		}
	}
}

//-----------------------------------------------------------------------------
// Links mouths
//-----------------------------------------------------------------------------

static void LinkMouths()
{
	for (int i = 0; i < g_nummouths; i++)
	{
		int j;
		for ( j = 0; j < g_numbones; j++)
		{
			if (g_mouth[i].bonename[0] && stricmp( g_mouth[i].bonename, g_bonetable[j].name) == 0)
				break;
		}
		if (j >= g_numbones)
		{
			MdlError("unknown mouth link '%s'\n", g_mouth[i].bonename );
		}
		g_mouth[i].bone = j;
	}
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
static float CalcPoseParameterValue( int control, RadianEuler &angle, Vector &pos )
{
	switch( control )
	{
	case STUDIO_X:
		return pos.x;
	case STUDIO_Y:
		return pos.y;
	case STUDIO_Z:
		return pos.z;
	case STUDIO_XR:
		return RAD2DEG( angle.x );
	case STUDIO_YR:
		return RAD2DEG( angle.y );
	case STUDIO_ZR:
		return RAD2DEG( angle.z );
	}
	return 0.0;
}

static void CalcPoseParameters( void )
{
	int i;
	matrix3x4_t boneToWorld[MAXSTUDIOBONES];
	RadianEuler angles;
	Vector pos;

	for (i = 0; i < g_sequence.Count(); i++)
	{
		s_sequence_t *pseq = &g_sequence[i];

		for (int iPose = 0; iPose < 2; iPose++)
		{
			if (pseq->groupsize[iPose] > 1)
			{
				if (pseq->paramattachment[iPose] != -1)
				{
					int j0 = pseq->paramindex[iPose];
					int n0 = pseq->paramattachment[iPose];
					int k0 = g_attachment[n0].bone;

					matrix3x4_t boneToWorldRel;
					matrix3x4_t boneToWorldMid;
					matrix3x4_t worldToBoneMid;
					matrix3x4_t boneRel;

					// printf("%s\n", pseq->name );

					if (pseq->paramanim == NULL)
					{
						pseq->paramanim = g_panimation[0];
					}

					if (pseq->paramcompanim == NULL)
					{
						pseq->paramcompanim = pseq->paramanim;
					}


					// calculate what "zero" looks like to the attachment
					CalcBoneTransforms( pseq->paramanim, 0, boneToWorld );
					ConcatTransforms( boneToWorld[k0], g_attachment[n0].local, boneToWorldMid );
					MatrixAngles( boneToWorldMid, angles, pos );
					// printf("%s : %s : %6.2f %6.2f %6.2f : %6.2f %6.2f %6.2f\n", pseq->name, g_pose[j0].name, RAD2DEG( angles.x ), RAD2DEG( angles.y ), RAD2DEG( angles.z ), pos.x, pos.y, pos.z );
					MatrixInvert( boneToWorldMid, worldToBoneMid );

					if ( g_verbose )
					{
						printf("%s : %s", pseq->name, g_pose[j0].name );
					}

					// for 2D animation, figure out what opposite row/column to use
					// FIXME: make these 2D instead of 2 1D!
					int m[2];
					bool found = false;
					if (pseq->paramcenter != NULL)
					{
						for (int i0 = 0; !found && i0 < pseq->groupsize[0]; i0++)
						{
							for (int i1 = 0; !found && i1 < pseq->groupsize[1]; i1++)
							{
								if (pseq->panim[i0][i1] == pseq->paramcenter)
								{
									m[0] = i0;
									m[1] = i1;
									found = true;
								}
							}
						}
					}
					if (!found)
					{
						m[1-iPose] = (pseq->groupsize[1-iPose]) / 2;
					}

					// find changes to attachment
					for (m[iPose] = 0; m[iPose] < pseq->groupsize[iPose]; m[iPose]++)
					{
						CalcBoneTransforms( pseq->panim[m[0]][m[1]], pseq->paramcompanim, 0, boneToWorld );
						ConcatTransforms( boneToWorld[k0], g_attachment[n0].local, boneToWorldRel );
						ConcatTransforms( worldToBoneMid, boneToWorldRel, boneRel );
						MatrixAngles( boneRel, angles, pos );
						// printf("%6.2f %6.2f %6.2f : %6.2f %6.2f %6.2f\n", RAD2DEG( angles.x ), RAD2DEG( angles.y ), RAD2DEG( angles.z ), pos.x, pos.y, pos.z );

						float v = CalcPoseParameterValue( pseq->paramcontrol[iPose], angles, pos );

						if ( g_verbose )
						{
							printf(" %6.2f", v );
						}

						if (iPose == 0)
						{
							pseq->param0[m[iPose]] = v;
						}
						else
						{
							pseq->param1[m[iPose]] = v;
						}


						// pseq->param1[i0][i1] = CalcPoseParameterValue( pseq->paramcontrol[1], angles, pos );

						if (m[iPose] == 0)
						{
							pseq->paramstart[iPose] = (iPose == 0) ? pseq->param0[m[iPose]] : pseq->param1[m[iPose]];
						}
						if (m[iPose] == pseq->groupsize[iPose] - 1)
						{
							pseq->paramend[iPose] = (iPose == 0) ? pseq->param0[m[iPose]] : pseq->param1[m[iPose]];
						}
					}

					if ( g_verbose )
					{
						printf("\n");
					}

					if (fabs( pseq->paramstart[iPose] - pseq->paramend[iPose]) < 0.01 )
					{
						MdlError( "calcblend failed in %s\n", pseq->name );
					}

					g_pose[j0].min = MIN( g_pose[j0].min, pseq->paramstart[iPose] );
					g_pose[j0].max = MAX( g_pose[j0].max, pseq->paramstart[iPose] );
					g_pose[j0].min = MIN( g_pose[j0].min, pseq->paramend[iPose] );
					g_pose[j0].max = MAX( g_pose[j0].max, pseq->paramend[iPose] );
				}
				else
				{

					for (int m = 0; m < pseq->groupsize[iPose]; m++)
					{
						float f = (m / (float)(pseq->groupsize[iPose] - 1.0));
						if (iPose == 0)
						{
							pseq->param0[m] = pseq->paramstart[iPose] * (1.0 - f) + pseq->paramend[iPose] * f;
						}
						else
						{
							pseq->param1[m] = pseq->paramstart[iPose] * (1.0 - f) + pseq->paramend[iPose] * f;
						}
					}
				}
			}
		}
	}
	// exit(0);
}



//-----------------------------------------------------------------------------
// Link ikchains
//-----------------------------------------------------------------------------

static void LinkIKChains( )
{
	int i, k;

	// create IK links
	for (i = 0; i < g_numikchains; i++)
	{
		g_ikchain[i].numlinks = 3;

		k = findGlobalBone( g_ikchain[i].bonename );
		if (k == -1)
		{
			MdlError("unknown bone '%s' in ikchain '%s'\n", g_ikchain[i].bonename, g_ikchain[i].name );
		}
		g_ikchain[i].link[2].bone = k;
		g_bonetable[k].flags |= BONE_USED_BY_ATTACHMENT;

		k = g_bonetable[k].parent;
		if (k == -1)
		{
			MdlError("ikchain '%s' too close to root, no parent knee/elbow\n", g_ikchain[i].name );
		}
		g_ikchain[i].link[1].bone = k;
		g_bonetable[k].flags |= BONE_USED_BY_ATTACHMENT;

		k = g_bonetable[k].parent;
		if (k == -1)
		{
			MdlError("ikchain '%s' too close to root, no parent hip/shoulder\n", g_ikchain[i].name );
		}
		g_ikchain[i].link[0].bone = k;
		g_bonetable[k].flags |= BONE_USED_BY_ATTACHMENT;

		// FIXME: search for toes
	}
}

//-----------------------------------------------------------------------------
// Link ikchains
//-----------------------------------------------------------------------------

static void LinkIKLocks( )
{
	int i, j;

	// create IK links
	for (i = 0; i < g_numikautoplaylocks; i++)
	{
		for (j = 0; j < g_numikchains; j++)
		{
			if (stricmp( g_ikchain[j].name, g_ikautoplaylock[i].name) == 0)
			{
				break;
			}
		}
		if (j == g_numikchains)
		{
			MdlError("unknown chain '%s' in ikautoplaylock\n", g_ikautoplaylock[i].name );
		}

		g_ikautoplaylock[i].chain = j;
	}

	int k;

	for (k = 0; k < g_sequence.Count(); k++)
	{
		for (i = 0; i < g_sequence[k].numiklocks; i++)
		{
			for (j = 0; j < g_numikchains; j++)
			{
				if (stricmp( g_ikchain[j].name, g_sequence[k].iklock[i].name) == 0)
				{
					break;
				}
			}
			if (j == g_numikchains)
			{
				MdlError("unknown chain '%s' in sequence iklock\n", g_sequence[k].iklock[i].name );
			}

			g_sequence[k].iklock[i].chain = j;
		}
	}
}

//-----------------------------------------------------------------------------
// Process IK links
//-----------------------------------------------------------------------------

s_ikrule_t *FindPrevIKRule( s_animation_t *panim, int iRule )
{
	int i, j;

	s_ikrule_t *pRule = &panim->ikrule[iRule];

	for (i = 1; i < panim->numikrules; i++)
	{
		j =  ( iRule - i + panim->numikrules) % panim->numikrules;
		if (panim->ikrule[j].chain == pRule->chain)
			return &panim->ikrule[j];
	}
	return pRule;
}

s_ikrule_t *FindNextIKRule( s_animation_t *panim, int iRule )
{
	int i, j;

	s_ikrule_t *pRule = &panim->ikrule[iRule];

	for (i = 1; i < panim->numikrules; i++)
	{
		j =  (iRule + i ) % panim->numikrules;
		if (panim->ikrule[j].chain == pRule->chain)
			return &panim->ikrule[j];
	}
	return pRule;
}



//-----------------------------------------------------------------------------
// Purpose: don't allow bones to change their length if they're predefined.
//			go through all the animations and reset them, but move anything on an ikchain back to where it was.
//-----------------------------------------------------------------------------
static void LockBoneLengths()
{
	int i, j, k;

	int n;

	if (!g_bLockBoneLengths)
		return;

	Vector origLocalPos[MAXSTUDIOBONES];

	// find original lengths
	for (k = 0; k < g_numbones; k++)
	{
		MatrixPosition( g_bonetable[k].rawLocalOriginal, origLocalPos[k] );

		if ( g_verbose )
		{
			Vector prev, delta;
			MatrixPosition( g_bonetable[k].rawLocal, prev );
			delta = prev - origLocalPos[k];
			printf("%s - %f %f %f\n", g_bonetable[k].name, delta.x, delta.y, delta.z );
		}
	}

	for (i = 0; i < g_numani; i++)
	{
		s_animation_t *panim = g_panimation[i];

		if (panim->flags & STUDIO_DELTA)
			continue;

		for (j = 0; j < panim->numframes; j++)
		{
			matrix3x4a_t boneToWorldOriginal[MAXSTUDIOBONES];
			matrix3x4a_t boneToWorld[MAXSTUDIOBONES];

			// calc original transformations
			CalcBoneTransforms( panim, j, boneToWorldOriginal );

			// force bones back to original lengths
			for (k = 0; k < g_numbones; k++)
			{
				if (g_bonetable[k].parent != -1)
				{
					//Vector delta = panim->sanim[j][k].pos - origLocalPos[k];
					//printf("%f %f %f\n", delta.x, delta.y, delta.z );
					panim->sanim[j][k].pos = origLocalPos[k];
				}
			}

			// calc new transformations
			CalcBoneTransforms( panim, j, boneToWorld );

			for (n = 0; n < g_numikchains; n++)
			{
				if (panim->weight[g_ikchain[n].link[2].bone] > 0)
				{
					Vector worldPos;
					MatrixPosition( boneToWorldOriginal[g_ikchain[n].link[2].bone], worldPos );

					Studio_SolveIK(
						g_ikchain[n].link[0].bone,
						g_ikchain[n].link[1].bone,
						g_ikchain[n].link[2].bone,
						worldPos,
						boneToWorld );

					solveBone( panim, j, g_ikchain[n].link[0].bone, boneToWorld );  
					solveBone( panim, j, g_ikchain[n].link[1].bone, boneToWorld );  
					solveBone( panim, j, g_ikchain[n].link[2].bone, boneToWorld );
				}
			}
		}
	}
}


void WrapToFrameRange( int &inputFrame, const s_animation_t *panim )
{
	inputFrame = (panim->numframes + inputFrame) % panim->numframes;
	if ( inputFrame < 0 )
		inputFrame += panim->numframes;
}

int SortPosAnim( const void *fl1, const void *fl2 )
{
	if ( *(const float *)fl1 >= *(const float *)fl2 )
		return 1;
	return -1;
}

struct s_footdown_t
{
	int nIndex;
	int nLength;
	
	s_footdown_t()
	{
		nIndex = -1;
		nLength = 0;
	}
};

//-----------------------------------------------------------------------------
// Purpose: go through all the IK rules and calculate the animated path the IK'd 
//			end point moves relative to its IK target.
//-----------------------------------------------------------------------------
static void ProcessIKRules( )
{
	int i, j, k;

	// copy source animations
	for (i = 0; i < g_numani; i++)
	{
		s_animation_t *panim = g_panimation[i];
		const char *pAnimationName = g_panimation[i]->animationname;
		s_sourceanim_t *pSourceAnim = FindSourceAnim( panim->source, pAnimationName );

		for (j = 0; j < panim->numcmds; j++)
		{
			if ( panim->cmds[j].cmd == CMD_IKFIXUP )
			{
				fixupIKErrors( panim, panim->cmds[j].u.ikfixup.pRule );
			}

			if (panim->cmds[j].cmd != CMD_IKRULE)
				continue;

			if (panim->numikrules >= MAXSTUDIOIKRULES)
			{
				MdlError("Too many IK rules in %s (%s)\n", panim->name, panim->filename );
			}
			s_ikrule_t *pRule = &panim->ikrule[panim->numikrules++];

			// make a copy of the rule;
			*pRule = *panim->cmds[j].u.ikrule.pRule;
			
			// -2 is a hack to tag the rule as 'auto-detect footsteps'
			if ( pRule->start == -2 )
			{
				// use the end var to store the step index
				if ( pRule->end > 1 )
				{
					for (k=1; k<pRule->end; k++)
					{
						s_ikrule_t *pRuleSub = &panim->ikrule[panim->numikrules++];

						// make a copy of the rule;
						*pRuleSub = *panim->cmds[j].u.ikrule.pRule;

						pRuleSub->peak = k;
					}
				}
				
				pRule->peak = 0;
			}

		}

		for (j = 0; j < panim->numikrules; j++)
		{
			s_ikrule_t *pRule = &panim->ikrule[j];

			if ( pRule->start == -2 )
			{
				// automatically tag footsteps

				//magic numbers
				float flModuloClamp = 0.3f;
				float flHeightFailsafe = 1;

				int nStepIndex = pRule->peak;
				int nNumSteps = pRule->end;

				matrix3x4_t boneToWorld[MAXSTUDIOBONES];
				int nBone = findGlobalBone( pRule->bonename );
				
				CUtlVector<float> vecPosAnim;
				CUtlVector<float> vecPosAnimSorted;
				
				int nHighestIndex = INT_MIN;
				float flHighestZ = FLT_MIN;

				// gather z positions, find the lowest z
				for ( int nFrame = 0; nFrame < panim->numframes; nFrame++ )
				{
					CalcBoneTransforms( panim, nFrame, boneToWorld );
					Vector vecTemp;
					MatrixPosition( boneToWorld[nBone], vecTemp );

					vecPosAnim.AddToTail( vecTemp.z - fmod( vecTemp.z, flModuloClamp ) );
					vecPosAnimSorted.AddToTail( vecTemp.z - fmod( vecTemp.z, flModuloClamp ) );

					if ( vecTemp.z > flHighestZ )
					{
						nHighestIndex = nFrame;
						flHighestZ = vecTemp.z;
					}
				}
				qsort( vecPosAnimSorted.Base(), vecPosAnimSorted.Count(), sizeof(float), SortPosAnim );
				
				// crawl up from the lowest z, finding the number of curve intersections. 
				// we want 2x nNumSteps intersections.

				bool bFoundSteps = false;
				float flCrawlHeight = vecPosAnimSorted[0];
				
				int nCrawlIndex = -1;

				for ( int nCrawl = 0; nCrawl < vecPosAnimSorted.Count(); nCrawl++ )
				{

					flCrawlHeight = vecPosAnimSorted[nCrawl];

					if ( bFoundSteps && flCrawlHeight > vecPosAnimSorted[0] + flHeightFailsafe )
						break;

					int nNumCurveIntersections = 0;
					for ( int nFrame = nHighestIndex; nFrame < panim->numframes+nHighestIndex; nFrame++ )
					{
						int nCurrent = nFrame;
						WrapToFrameRange(nCurrent, panim);
						int nNext = nFrame+1;
						WrapToFrameRange(nNext, panim);
						if ( (vecPosAnim[nCurrent] > flCrawlHeight && vecPosAnim[nNext] <= flCrawlHeight) || (vecPosAnim[nCurrent] <= flCrawlHeight && vecPosAnim[nNext] > flCrawlHeight) )
						{
							nNumCurveIntersections++;
						}
					}

					if ( nNumCurveIntersections == nNumSteps * 2 )
					{
						bFoundSteps = true;
						nCrawlIndex = nCrawl;
					}

					if ( bFoundSteps && nNumCurveIntersections != nNumSteps * 2 )
						break;

				}

				Assert( nCrawlIndex != -1 );
				if ( nCrawlIndex == -1 )
				{
					//for ( int nFrame = 0; nFrame < panim->numframes; nFrame++ )
					//{
					//	char szTemp[128] = "";
					//	for ( int cc=0; cc<vecPosAnim[nFrame] * 5; cc++ )
					//	{
					//		V_strcat_safe( szTemp, "*" );
					//	}					
					//	Msg( "%s\n", szTemp );
					//}

					MdlError( "Failed to detect exactly %i footsteps in %s.\n", nNumSteps, panim->name );
				}

				// extract footdowns from the last successful crawlheight
				CUtlVector<s_footdown_t> vecFootDowns;
				vecFootDowns.RemoveAll();
				s_footdown_t temp;

				flCrawlHeight = vecPosAnimSorted[nCrawlIndex];

				for ( int nFrame = nHighestIndex; nFrame < panim->numframes+nHighestIndex; nFrame++ )
				{
					int nCurrent = nFrame;
					WrapToFrameRange(nCurrent, panim);
					int nNext = nFrame+1;
					WrapToFrameRange(nNext, panim);

					if ( (vecPosAnim[nCurrent] > flCrawlHeight && vecPosAnim[nNext] <= flCrawlHeight) )
					{
						temp.nIndex = nCurrent;
						temp.nLength = 0;
					}

					if ( vecPosAnim[nCurrent] <= flCrawlHeight )
					{
						temp.nLength++;
					}
				
					if ( (vecPosAnim[nCurrent] <= flCrawlHeight && vecPosAnim[nNext] > flCrawlHeight) )
					{
						vecFootDowns.AddToTail(temp);
					}
				}

				bool bSuccess = ( bFoundSteps && vecFootDowns.Count() > 0 && vecFootDowns.Count() == nNumSteps && vecFootDowns.Count() > nStepIndex );
				Assert( bSuccess );

				if ( !bSuccess )
					MdlError( "Failed to detect footsteps in %s.\n", panim->name );

				s_footdown_t FootDown = vecFootDowns[ nStepIndex ];

				int nFootDownFrame = vecFootDowns[ nStepIndex ].nIndex;
				int nFootDownDuration = vecFootDowns[ nStepIndex ].nLength;
				
				//Msg( "Detected footstep (%s) on frame %i of %s. Step #(%i).\n", pRule->bonename, nFootDownFrame, panim->name, nStepIndex );
				
				pRule->start = nFootDownFrame;
				pRule->peak = nFootDownFrame + (int)(nFootDownDuration * 0.2f);
				pRule->tail = nFootDownFrame + (int)(nFootDownDuration * 0.8f);
				pRule->end  = nFootDownFrame + (int)(nFootDownDuration * 1.0f);
				
				WrapToFrameRange( pRule->start, panim );
				WrapToFrameRange( pRule->peak, panim );
				WrapToFrameRange( pRule->tail, panim );
				WrapToFrameRange( pRule->end, panim );

				//for ( int nFrame = 0; nFrame < panim->numframes; nFrame++ )
				//{
				//	char szTemp[128] = "";
				//
				//	for ( int cc=0; cc<vecPosAnim[nFrame] * 5; cc++ )
				//	{
				//		V_strcat_safe( szTemp, "*" );
				//	}
				//
				//	if ( nFrame == pRule->start )
				//		V_strcat_safe( szTemp, "<-start--------" );
				//
				//	if ( nFrame == pRule->peak )
				//		V_strcat_safe( szTemp, "<-peak---------" );
				//
				//	if ( nFrame == pRule->tail )
				//		V_strcat_safe( szTemp, "<-tail---------" );
				//
				//	if ( nFrame == pRule->end )
				//		V_strcat_safe( szTemp, "<-end----------" );
				//
				//	Msg( "%s\n", szTemp );
				//}
				//Msg( "************************\n" );

			}


			if (pRule->start == 0 && pRule->peak == 0 && pRule->tail == 0 && pRule->end == 0)
			{
				pRule->tail = panim->numframes - 1;
				pRule->end = panim->numframes - 1;
			}

			if (pRule->start != -1 && pRule->peak == -1 && pRule->tail == -1 && pRule->end != -1)
			{
				pRule->peak = (pRule->start + pRule->end) / 2;
				pRule->tail = (pRule->start + pRule->end) / 2;
			}

			if (pRule->start != -1 && pRule->peak == -1 && pRule->tail != -1)
			{
				pRule->peak = (pRule->start + pRule->tail) / 2;
			}

			if (pRule->peak != -1 && pRule->tail == -1 && pRule->end != -1)
			{
				pRule->tail = (pRule->peak + pRule->end) / 2;
			}

			if (pRule->peak == -1)
			{
				pRule->start = 0;
				pRule->peak = 0;
			}

			if (pRule->tail == -1)
			{
				pRule->tail = panim->numframes - 1;
				pRule->end = panim->numframes - 1;
			}

			if (pRule->contact == -1)
			{
				pRule->contact = pRule->peak;
			}

			// huh, make up start and end numbers
			if (pRule->start == -1)
			{
				s_ikrule_t *pPrev = FindPrevIKRule( panim, j );

				if (pPrev->slot == pRule->slot)
				{
					if (pRule->peak < pPrev->tail)
					{
						pRule->start = pRule->peak + (pPrev->tail - pRule->peak) / 2;
					}
					else
					{
						pRule->start = pRule->peak + (pPrev->tail - pRule->peak + panim->numframes - 1) / 2;
					}
					pRule->start = (pRule->start + panim->numframes / 2) % (panim->numframes - 1);
					pPrev->end = (pRule->start + panim->numframes - 1) % (panim->numframes - 1);
				}
				else
				{
					pRule->start = pPrev->tail;
					pPrev->end = pRule->peak;
				}
				// printf("%s : %d (%d) : %d %d %d %d\n", panim->name, pRule->chain, panim->numframes - 1, pRule->start, pRule->peak, pRule->tail, pRule->end );
			}

			// huh, make up start and end numbers
			if (pRule->end == -1)
			{
				s_ikrule_t *pNext = FindNextIKRule( panim, j );

				if (pNext->slot == pRule->slot)
				{
					if (pNext->peak < pRule->tail)
					{
						pNext->start = pNext->peak + (pRule->tail - pNext->peak) / 2;
					}
					else
					{
						pNext->start = pNext->peak + (pRule->tail - pNext->peak + panim->numframes - 1) / 2;
					}
					pNext->start = (pNext->start + panim->numframes / 2) % (panim->numframes - 1);
					pRule->end = (pNext->start + panim->numframes - 1) % (panim->numframes - 1);
				}
				else
				{
					pNext->start = pRule->tail;
					pRule->end = pNext->peak;
				}
				// printf("%s : %d (%d) : %d %d %d %d\n", panim->name, pRule->chain, panim->numframes - 1, pRule->start, pRule->peak, pRule->tail, pRule->end );
			}

			// check for wrapping
			if (pRule->peak < pRule->start)
			{
				pRule->peak += panim->numframes - 1;
			}
			if (pRule->tail < pRule->peak)
			{
				pRule->tail += panim->numframes - 1;
			}
			if (pRule->end < pRule->tail)
			{
				pRule->end += panim->numframes - 1;
			}
			if (pRule->contact < pRule->start)
			{
				pRule->contact += panim->numframes - 1;
			}

			/*
			printf("%s : %d (%d) : %d %d %d %d : %s\n", panim->name, pRule->chain, panim->numframes - 1, pRule->start, pRule->peak, pRule->tail, pRule->end,
				pRule->usesequence ? "usesequence" : pRule->usesource ? "source" : "" );
			*/

			pRule->errorData.numerror = pRule->end - pRule->start + 1;
			if (pRule->end >= panim->numframes)
				pRule->errorData.numerror = pRule->errorData.numerror + 2;

			pRule->errorData.pError = (s_streamdata_t *)calloc( pRule->errorData.numerror, sizeof( s_streamdata_t ));

			int n = 0;

			if (pRule->usesequence)
			{
				// FIXME: bah, this is horrendously hacky, add a damn back pointer
				for (n = 0; n < g_sequence.Count(); n++)
				{
					if (g_sequence[n].panim[0][0] == panim)
						break;
				}
			}

			switch( pRule->type )
			{
			case IK_SELF:
				{
					matrix3x4_t boneToWorld[MAXSTUDIOBONES];
					matrix3x4_t worldToBone;
					matrix3x4_t local;

					if (strlen(pRule->bonename) == 0)
					{
						pRule->bone = -1;
					}
					else
					{

						pRule->bone = findGlobalBone( pRule->bonename );
						if (pRule->bone == -1)
						{
							MdlError("unknown bone '%s' in ikrule\n", pRule->bonename );
						}
					}

					for (k = 0; k < pRule->errorData.numerror; k++)
					{
						if (pRule->usesequence)
						{
							CalcSeqTransforms( n, k + pRule->start, boneToWorld );
						}
						else if (pRule->usesource)
						{
							matrix3x4_t srcBoneToWorld[MAXSTUDIOSRCBONES];
							BuildRawTransforms( panim->source, pAnimationName, k + pRule->start + panim->startframe - pSourceAnim->startframe, panim->scale, panim->adjust, panim->rotation, panim->flags, srcBoneToWorld );
							TranslateAnimations( panim->source, srcBoneToWorld, boneToWorld );
						}
						else 
						{
							CalcBoneTransforms( panim, k + pRule->start, boneToWorld );
						}


						if (pRule->bone != -1)
						{
							MatrixInvert( boneToWorld[pRule->bone], worldToBone );
							ConcatTransforms( worldToBone, boneToWorld[g_ikchain[pRule->chain].link[2].bone], local );
						}
						else
						{
							MatrixCopy( boneToWorld[g_ikchain[pRule->chain].link[2].bone], local );
						}

						MatrixAngles( local, pRule->errorData.pError[k].q, pRule->errorData.pError[k].pos );

						/*
						QAngle ang;
						QuaternionAngles( pRule->errorData.pError[k].q, ang );
						printf("%d  %.1f %.1f %.1f : %.1f %.1f %.1f\n", 
							k,
							pRule->errorData.pError[k].pos.x, pRule->errorData.pError[k].pos.y, pRule->errorData.pError[k].pos.z, 
							ang.x, ang.y, ang.z );
						*/
					}
				}
				break;
			case IK_WORLD:
				break;
			case IK_ATTACHMENT:
				{
					matrix3x4_t boneToWorld[MAXSTUDIOBONES];
					matrix3x4_t worldToBone;
					matrix3x4_t local;

					int bone = g_ikchain[pRule->chain].link[2].bone;
					CalcBoneTransforms( panim, pRule->contact, boneToWorld );
					// FIXME: add in motion

					// pRule->pos = footfall;
					// pRule->q = RadianEuler( 0, 0, 0 );

					if (strlen(pRule->bonename) == 0)
					{
						if (pRule->bone != -1)
						{
							pRule->bone = bone;
						}
					}
					else
					{
						pRule->bone = findGlobalBone( pRule->bonename );
						if (pRule->bone == -1)
						{
							MdlError("unknown bone '%s' in ikrule\n", pRule->bonename );
						}
					}

					if (pRule->bone != -1)
					{
						// FIXME: look for local bones...
						CalcBoneTransforms( panim, pRule->contact, boneToWorld );
						MatrixAngles( boneToWorld[pRule->bone], pRule->q, pRule->pos );
					}

#if 0
					printf("%d  %.1f %.1f %.1f\n", 
						pRule->peak,
						pRule->pos.x, pRule->pos.y, pRule->pos.z );
#endif

					for (k = 0; k < pRule->errorData.numerror; k++)
					{
						int t = k + pRule->start;

						if (pRule->usesequence)
						{
							CalcSeqTransforms( n, t, boneToWorld );
						}
						else if (pRule->usesource)
						{
							matrix3x4_t srcBoneToWorld[MAXSTUDIOSRCBONES];
							BuildRawTransforms( panim->source, pAnimationName, t + panim->startframe - pSourceAnim->startframe, srcBoneToWorld );
							TranslateAnimations( panim->source, srcBoneToWorld, boneToWorld );
						}
						else 
						{
							CalcBoneTransforms( panim, t, boneToWorld );
						}

						Vector pos = pRule->pos + calcMovement( panim, t, pRule->contact );

						// printf("%2d : %2d : %4.2f %6.1f %6.1f %6.1f\n", k, t, s, pos.x, pos.y, pos.z );


						AngleMatrix( RadianEuler( pRule->q ), pos, local );
						MatrixInvert( local, worldToBone );

						// calc position error
						ConcatTransforms( worldToBone, boneToWorld[bone], local );
						MatrixAngles( local, pRule->errorData.pError[k].q, pRule->errorData.pError[k].pos );

#if 0
						QAngle ang;
						QuaternionAngles( pRule->errorData.pError[k].q, ang );
						printf("%d  %.1f %.1f %.1f : %.1f %.1f %.1f\n", 
							k + pRule->start,
							pRule->errorData.pError[k].pos.x, pRule->errorData.pError[k].pos.y, pRule->errorData.pError[k].pos.z, 
							ang.x, ang.y, ang.z );
#endif
					}
				}
				break;
			case IK_GROUND:
				{
					matrix3x4_t boneToWorld[MAXSTUDIOBONES];
					matrix3x4_t worldToBone;
					matrix3x4_t local;

					int bone = g_ikchain[pRule->chain].link[2].bone;

					if (pRule->usesequence)
					{
						CalcSeqTransforms( n, pRule->contact, boneToWorld );
					}
					else if (pRule->usesource)
					{
						matrix3x4_t srcBoneToWorld[MAXSTUDIOSRCBONES];
						BuildRawTransforms( panim->source, pAnimationName, pRule->contact + panim->startframe - pSourceAnim->startframe, panim->scale, panim->adjust, panim->rotation, panim->flags, srcBoneToWorld );
						TranslateAnimations( panim->source, srcBoneToWorld, boneToWorld );
					}
					else 
					{
						CalcBoneTransforms( panim, pRule->contact, boneToWorld );
					}

					// FIXME: add in motion

					Vector footfall;
					VectorTransform( g_ikchain[pRule->chain].center, boneToWorld[bone], footfall );
					footfall.z = pRule->floor;

					AngleMatrix( RadianEuler( 0, 0, 0 ), footfall, local );
					MatrixInvert( local, worldToBone );

					pRule->pos = footfall;
					pRule->q = Quaternion( RadianEuler( 0, 0, 0 ) );
					
#if 0
					printf("%d  %.1f %.1f %.1f\n", 
						pRule->peak,
						pRule->pos.x, pRule->pos.y, pRule->pos.z );
#endif

					float s;
					for (k = 0; k < pRule->errorData.numerror; k++)
					{
						int t = k + pRule->start;
						/*
						if (t > pRule->end)
						{
							t = t - (panim->numframes - 1);
						}
						*/

						if (pRule->usesequence)
						{
							CalcSeqTransforms( n, t, boneToWorld );
						}
						else if (pRule->usesource)
						{
							matrix3x4_t srcBoneToWorld[MAXSTUDIOSRCBONES];
							BuildRawTransforms( panim->source, pAnimationName, pRule->contact + panim->startframe - pSourceAnim->startframe, panim->scale, panim->adjust, panim->rotation, panim->flags, srcBoneToWorld );
							TranslateAnimations( panim->source, srcBoneToWorld, boneToWorld );
						}
						else 
						{
							CalcBoneTransforms( panim, t, boneToWorld );
						}
						Vector pos = pRule->pos + calcMovement( panim, t, pRule->contact );
						s = 0.0;

						Vector cur;
						VectorTransform( g_ikchain[pRule->chain].center, boneToWorld[bone], cur );
						cur.z = pos.z;

						if (t < pRule->start || t >= pRule->end)
						{
							// s = (float)(t - pRule->start) / (pRule->peak - pRule->start);
							// pos = startPos * (1 - s) + pos * s;
							pos = cur;
						}
						else if (t < pRule->peak)
						{
							s = (float)(pRule->peak - t) / (pRule->peak - pRule->start);
							s = 3 * s * s - 2 * s * s * s;
							pos = pos * (1 - s) + cur * s;
						}
						else if (t > pRule->tail)
						{
							s = (float)(t - pRule->tail) / (pRule->end - pRule->tail);
							s = 3 * s * s - 2 * s * s * s;
							pos = pos * (1 - s) + cur * s;
							//pos = endPos - calcMovement( panim, t, pRule->tail );
						}

						//MatrixPosition( boneToWorld[bone], pos );
						//pos.z = pRule->floor;

						// printf("%2d : %2d : %4.2f %6.1f %6.1f %6.1f\n", k, t, s, pos.x, pos.y, pos.z );


						AngleMatrix( RadianEuler( pRule->q ), pos, local );
						MatrixInvert( local, worldToBone );

						// calc position error
						ConcatTransforms( worldToBone, boneToWorld[bone], local );
						MatrixAngles( local, pRule->errorData.pError[k].q, pRule->errorData.pError[k].pos );

#if 0
						QAngle ang;
						QuaternionAngles( pRule->errorData.pError[k].q, ang );
						printf("%d  %.1f %.1f %.1f : %.1f %.1f %.1f\n", 
							k + pRule->start,
							pRule->errorData.pError[k].pos.x, pRule->errorData.pError[k].pos.y, pRule->errorData.pError[k].pos.z, 
							ang.x, ang.y, ang.z );
#endif
					}
				}
				break;
			case IK_RELEASE:
			case IK_UNLATCH:
				break;
			}
		}

		if ((panim->flags & STUDIO_DELTA) || panim->noAutoIK)
			continue;

		// auto release ik chains that are moved but not referenced and have no explicit rules
		int count[16];

		for (j = 0; j < g_numikchains; j++)
		{
			count[j] = 0;
		}

		for (j = 0; j < panim->numikrules; j++)
		{
			count[panim->ikrule[j].chain]++;
		}

		for (j = 0; j < g_numikchains; j++)
		{
			if (count[j] == 0 && panim->weight[g_ikchain[j].link[2].bone] > 0.0)
			{
				// printf("%s - %s\n", panim->name, g_ikchain[j].name );
				k = panim->numikrules++;
				panim->ikrule[k].chain = j;
				panim->ikrule[k].slot = j;
				panim->ikrule[k].type = IK_RELEASE;
				panim->ikrule[k].start = 0;
				panim->ikrule[k].peak = 0;
				panim->ikrule[k].tail = panim->numframes - 1;
				panim->ikrule[k].end = panim->numframes - 1;
			}
		}
	}
	// exit(0);


	// realign IK across multiple animations
	for (i = 0; i < g_sequence.Count(); i++)
	{
		for (j = 0; j < g_sequence[i].groupsize[0]; j++)
		{
			for (k = 0; k < g_sequence[i].groupsize[1]; k++)
			{
				g_sequence[i].numikrules = MAX( g_sequence[i].numikrules, g_sequence[i].panim[j][k]->numikrules );
			}
		}

		// check for mismatched ik rules
		s_animation_t *panim1 = g_sequence[i].panim[0][0];
		for (j = 0; j < g_sequence[i].groupsize[0]; j++)
		{
			for (k = 0; k < g_sequence[i].groupsize[1]; k++)
			{
				s_animation_t *panim2 = g_sequence[i].panim[j][k];
				if (panim1->numikrules != panim2->numikrules)
				{
					MdlWarning( "%s - mismatched number of IK rules: \"%s\"[%i] \"%s\"[%i]\n", 
						g_sequence[i].name, panim1->name, panim1->numikrules, panim2->name, panim2->numikrules );

					s_animation_t *panim_from;
					s_animation_t *panim_to;

					if ( panim1->numikrules > panim2->numikrules )
					{
						panim_from = panim1;
						panim_to = panim2;
					}
					else
					{
						panim_from = panim2;
						panim_to = panim1;
					}

					panim_to->numikrules = panim_from->numikrules;

					for (int n = 0; n < panim_from->numikrules; n++)
					{
						panim_to->ikrule[n].type = panim_from->ikrule[n].type;
						panim_to->ikrule[n].chain = panim_from->ikrule[n].chain;
						panim_to->ikrule[n].slot = panim_from->ikrule[n].slot;
					}

				}
				for (int n = 0; n < panim1->numikrules; n++)
				{
					if ((panim1->ikrule[n].type != panim2->ikrule[n].type) ||
						(panim1->ikrule[n].chain != panim2->ikrule[n].chain) ||
						(panim1->ikrule[n].slot != panim2->ikrule[n].slot))
					{
						MdlError( "%s - mismatched IK rule %d: \n\"%s\" : %d %d %d\n\"%s\" : %d %d %d\n", 
							g_sequence[i].name, n, 
							panim1->name, panim1->ikrule[n].type, panim1->ikrule[n].chain, panim1->ikrule[n].slot,
							panim2->name, panim2->ikrule[n].type, panim2->ikrule[n].chain, panim2->ikrule[n].slot );
					}
				}
			}
		}

		// FIXME: this doesn't check alignment!!!
		for (j = 0; j < g_sequence[i].groupsize[0]; j++)
		{
			for (k = 0; k < g_sequence[i].groupsize[1]; k++)
			{
				for (int n = 0; n < g_sequence[i].panim[j][k]->numikrules; n++)
				{
					g_sequence[i].panim[j][k]->ikrule[n].index = n;
				}
			}
		}
	}
}


//-----------------------------------------------------------------------------
// CompressAnimations
//-----------------------------------------------------------------------------

static void CompressAnimations()
{
	// 2025
	// IDK HOW IT WORKS, BUT IT STILL USES EULERS
	// UPDATE: I REMOVE SOME SHIT. Probably this func should be removed.


	int i, j, k, n, m;


	// !!!
	//g_minSectionFrameLimit = 100000;
	//g_animblocksize = 0;

	// For g_bonetable[j].rot
	RadianEuler j_rot;
	// For g_panimation[i]->sanim[n][j].rot[k - 3];
	RadianEuler n_j_rot;

	// find scales for all bones
	for (j = 0; j < g_numbones; j++)
	{
		for (k = 0; k < 6; k++)
		{
			for (i = 0; i < g_numani; i++)
			{
				for (n = 0; n < g_panimation[i]->numframes; n++)
				{					
				}
			}			
		}
	}


	// вот эта хуйня отвечает за финальную запись анимаций в контексте simplify
	for (i = 0; i < g_numani; i++)
	{
		s_animation_t* panim = g_panimation[i];
		s_source_t* psource = panim->source;


		s_sourceanim_t* RawAnim = &psource->m_Animations[0];
		if (g_bCheckLengths)
		{
			printf("%s\n", panim->name);
		}

		// setup animation interior sections
		int iSectionFrames = panim->numframes;
		if (panim->numframes >= g_minSectionFrameLimit && false)
		{
			iSectionFrames = g_sectionFrames;
			panim->sectionframes = g_sectionFrames;
			panim->numsections = (int)(panim->numframes / panim->sectionframes) + 2;
		}
		else
		{
			panim->sectionframes = 0;
			panim->numsections = 1;
		}

		RadianEuler psrcdata_rot;
		RadianEuler g_bonetable_j;
		printf("panim->numsections [%s] %d [frames %d] \n", panim->name,panim->numsections, iSectionFrames);

		for (int w = 0; w < panim->numsections; w++)
		{
			int iStartFrame = w * iSectionFrames;
			int iEndFrame = (w + 1) * iSectionFrames;

			iStartFrame = MIN(iStartFrame, panim->numframes - 1);
			iEndFrame = MIN(iEndFrame, panim->numframes - 1);

			// printf("%s : %d %d\n", panim->name, iStartFrame, iEndFrame );

			for (j = 0; j < g_numbones; j++)
			{

				QuaternionAngles(g_bonetable[j].qrot, g_bonetable_j);

				for (k = 0; k < 6; k++)
				{
					//panim->anim[w][j].num[k] = 0;
					//panim->anim[w][j].data[k] = NULL;
				}

				// skip bones that are always procedural
				if (g_bonetable[j].flags & BONE_ALWAYS_PROCEDURAL)
				{
					// panim->weight[j] = 0.0;
					continue;
				}

				// skip bones that have no influence
				if (panim->weight[j] < 0.001)
					continue;

			
				for (k = 0; k < 6; k++)
				{
					mstudioanimvalue_t* pcount, * pvalue;
					float v;
					//short value[MAXSTUDIOANIMFRAMES];
					mstudioanimvalue_t data[MAXSTUDIOANIMFRAMES];

					// find deltas from default pose
					for (n = 0; n <= iEndFrame - iStartFrame; n++)
					{
					}

					if (n == 0)
						MdlError("no animation frames: \"%s\"\n", psource->filename);

					// FIXME: this compression algorithm needs work

					// initialize animation RLE block
					memset(data, 0, sizeof(data));
					pcount = data;
					pvalue = pcount + 1;

					pcount->num.valid = 1;
					pcount->num.total = 1;
//					pvalue->value = value[0];
					pvalue++;

					// build a RLE of deltas from the default pose
					for (m = 1; m < n; m++)
					{
						if (false)
						//if (pcount->num.total == 255)
						{
							// chain too long, force a new entry
							pcount = pvalue;
							pvalue = pcount + 1;
							pcount->num.valid++;
							//pvalue->value = value[m];
							pvalue++;
						}
						// insert value if they're not equal, 
						// or if we're not on a run and the run is less than 3 units
						else if (false)
						//else if ((value[m] != value[m - 1]) || ((pcount->num.total == pcount->num.valid) && ((m < n - 1) && value[m] != value[m + 1])))
						{
							if (pcount->num.total != pcount->num.valid)
							{
								//if (j == 0) printf("%d:%d   ", pcount->num.valid, pcount->num.total ); 
								pcount = pvalue;
								pvalue = pcount + 1;
							}
							pcount->num.valid++;
							//pvalue->value = value[m];
							pvalue++;
						}
						pcount->num.total++;
					}
					//if (j == 0) printf("%d:%d\n", pcount->num.valid, pcount->num.total ); 

					
					panim->anim[w][j].num[k] = pvalue - data;

					//pcount->num.valid = 100;
					//pcount->num.total = 100;
					
					//printf("Num %d \n", panim->anim[w][j].num[k]);

					if (false)
					//if (panim->anim[w][j].num[k] == 2 && value[0] == 0)
					{
						panim->anim[w][j].num[k] = 0;
					}
					else
					{
						panim->anim[w][j].data[k] = (mstudioanimvalue_t*)calloc(pvalue - data, sizeof(mstudioanimvalue_t));
						memmove(panim->anim[w][j].data[k], data, (pvalue - data) * sizeof(mstudioanimvalue_t));
					}
					// printf("%d(%d) ", g_source[i]->panim[q]->numanim[j][k], n );
				}
				
			}
		}

		if (panim->numsections == 1)
		{
			panim->sectionframes = 0;
		}
	}
}



static void oCompressAnimations( )
{
	// 2025
	// IDK HOW IT WORKS, BUT IT STILL USES EULERS


	int i, j, k, n, m;


	// !!!
	//g_minSectionFrameLimit = 100000;
	//g_animblocksize = 0;

	// For g_bonetable[j].rot
	RadianEuler j_rot;
	// For g_panimation[i]->sanim[n][j].rot[k - 3];
	RadianEuler n_j_rot;
	
	// find scales for all bones
	for (j = 0; j < g_numbones; j++)
	{

		// Temporary Implement for g_bonetable[j].rot (2025)
		QuaternionAngles(g_bonetable[j].qrot, j_rot);

		// printf("%s : ", g_bonetable[j].name );
		for (k = 0; k < 6; k++)
		{
			float minv, maxv, scale;
			float total_minv, total_maxv;

			if (k < 3) 
			{
				minv = -128.0;
				maxv = 128.0;
				total_maxv = total_minv = g_bonetable[j].pos[k];
			}
			else
			{
				minv = -M_PI / 8.0;
				maxv = M_PI / 8.0;
				
				// replace this to temp ang from Quaternion (2025)
				//total_maxv = total_minv = g_bonetable[j].rot[k-3];
					
				
				total_maxv = total_minv = j_rot[k - 3];
			}

			for (i = 0; i < g_numani; i++)
			{
				for (n = 0; n < g_panimation[i]->numframes; n++)
				{
					// for (v = g_panimation[i]->sanim[n][j].rot[k-3])
					QuaternionAngles(g_panimation[i]->sanim[n][j].qrot, n_j_rot);

					float v = 0.0f;
					switch(k)
					{
					case 0: 
					case 1: 
					case 2: 
						if (g_panimation[i]->flags & STUDIO_DELTA)
						{
							v = g_panimation[i]->sanim[n][j].pos[k]; 
						}
						else
						{
							v = ( g_panimation[i]->sanim[n][j].pos[k] - g_bonetable[j].pos[k] ); 

							if (g_panimation[i]->sanim[n][j].pos[k] < total_minv)
								total_minv = g_panimation[i]->sanim[n][j].pos[k];
							if (g_panimation[i]->sanim[n][j].pos[k] > total_maxv)
								total_maxv = g_panimation[i]->sanim[n][j].pos[k];
						}
						break;
					case 3:
					case 4:
					case 5:
						if (g_panimation[i]->flags & STUDIO_DELTA)
						{
							//v = g_panimation[i]->sanim[n][j].rot[k-3]; 
							v = n_j_rot[k-3];
						}
						else
						{
							v = (n_j_rot[k-3] - j_rot[k-3] );
						}
						while (v >= M_PI)
							v -= M_PI * 2;
						while (v < -M_PI)
							v += M_PI * 2;
						break;
					}
					if (v < minv)
						minv = v;
					if (v > maxv)
						maxv = v;
				}
			}
			if (minv < maxv)
			{
				if (-minv> maxv)
				{
					scale = minv / -32768.0;
				}
				else
				{
					scale = maxv / 32767;
				}
			}
			else
			{
				scale = 1.0 / 32.0;
			}

			//printf("scale %f\n", scale);

			// probably we dont need real scaling for saving proportions? (one value for all frames to prevent disagreements) (2025)
			//scale = 1/32;
			// 
			//scale = 1/32;
			scale = 0.01;

			switch(k)
			{
			case 0: 
			case 1: 
			case 2: 
				g_bonetable[j].posscale[k] = scale;
				g_bonetable[j].posrange[k] = 1;// total_maxv - total_minv;
				break;
			case 3:
			case 4:
			case 5:
				// printf("(%.1f %.1f)", RAD2DEG(minv), RAD2DEG(maxv) );
				// printf("(%.1f)", RAD2DEG(maxv-minv) );
				g_bonetable[j].rotscale[k-3] = scale;
				break;
			}
			// printf("%.0f ", 1.0 / scale );
		}
		// printf("\n" );
	}


	// reduce animations
	
	
	// вот эта хуйня отвечает за финальную запись анимаций в контексте simplify
	for (i = 0; i < g_numani; i++)
	{
		s_animation_t *panim = g_panimation[i];
		s_source_t *psource = panim->source;


		s_sourceanim_t* RawAnim = &psource->m_Animations[0];
		if (g_bCheckLengths)
		{
			printf("%s\n", panim->name ); 
		}

		// setup animation interior sections
		int iSectionFrames = panim->numframes;
		if ( panim->numframes >= g_minSectionFrameLimit )
		{
			iSectionFrames = g_sectionFrames;
			panim->sectionframes = g_sectionFrames;
			panim->numsections = (int)(panim->numframes / panim->sectionframes) + 2;
		}
		else
		{
			panim->sectionframes = 0;
			panim->numsections = 1;
		}

		RadianEuler psrcdata_rot;
		RadianEuler g_bonetable_j;
		for (int w = 0; w < panim->numsections; w++)
		{
			int iStartFrame = w * iSectionFrames;
			int iEndFrame = (w + 1) * iSectionFrames;

			iStartFrame = MIN( iStartFrame, panim->numframes - 1 );
			iEndFrame = MIN( iEndFrame, panim->numframes - 1 );

			// printf("%s : %d %d\n", panim->name, iStartFrame, iEndFrame );

			for (j = 0; j < g_numbones; j++)
			{
				
				QuaternionAngles(g_bonetable[j].qrot, g_bonetable_j);

				for (k = 0; k < 6; k++)
				{
					panim->anim[w][j].num[k] = 0;
					panim->anim[w][j].data[k] = NULL;
				}

				// skip bones that are always procedural
				if (g_bonetable[j].flags & BONE_ALWAYS_PROCEDURAL)
				{
					// panim->weight[j] = 0.0;
					continue;
				}

				// skip bones that have no influence
				if (panim->weight[j] < 0.001)
					continue;

				int checkmin[6], checkmax[6];
				for (k = 0; k < 6; k++)
				{
					checkmin[k] = 32767;
					checkmax[k] = -32768;
				}

				for (k = 0; k < 6; k++)
				{
					mstudioanimvalue_t	*pcount, *pvalue;
					float v;
					short value[MAXSTUDIOANIMFRAMES];
					mstudioanimvalue_t data[MAXSTUDIOANIMFRAMES];

					// find deltas from default pose
					for (n = 0; n <= iEndFrame - iStartFrame; n++)
					{
						s_bone_t *psrcdata = &panim->sanim[n+iStartFrame][j];
						
						QuaternionAngles(psrcdata->qrot, psrcdata_rot);
						


						switch(k)
						{
						case 0: /* X Position */
						case 1: /* Y Position */
						case 2: /* Z Position */
							if (panim->flags & STUDIO_DELTA)
							{
								value[n] = psrcdata->pos[k] / g_bonetable[j].posscale[k]; 
								// pre-scale pos delta since format only has room for "overall" weight
								float r = panim->posweight[j] / panim->weight[j];
								value[n] *= r;
							}
							else
							{
								value[n] = ( psrcdata->pos[k] - g_bonetable[j].pos[k] ) / g_bonetable[j].posscale[k]; 
							}

							break;
						case 3: /* X Rotation */
						case 4: /* Y Rotation */
						case 5: /* Z Rotation */
							if (panim->flags & STUDIO_DELTA)
							{
								v = psrcdata_rot[k-3];
							}
							else
							{
								v = (psrcdata_rot[k-3] - g_bonetable_j[k-3] );
							}

							while (v >= M_PI)
								v -= M_PI * 2;
							while (v < -M_PI)
								v += M_PI * 2;

							value[n] = v / g_bonetable[j].rotscale[k-3]; 
							break;
						}
						checkmin[k] = MIN( value[n], checkmin[k] );
						checkmax[k] = MAX( value[n], checkmax[k] );
					}
					if (n == 0)
						MdlError("no animation frames: \"%s\"\n", psource->filename );

					// FIXME: this compression algorithm needs work

					// initialize animation RLE block
					memset( data, 0, sizeof( data ) ); 
					pcount = data; 
					pvalue = pcount + 1;

					pcount->num.valid = 1;
					pcount->num.total = 1;
					pvalue->value = value[0];
					pvalue++;

					// build a RLE of deltas from the default pose
					for (m = 1; m < n; m++)
					{
						if (pcount->num.total == 255)
						{
							// chain too long, force a new entry
							pcount = pvalue;
							pvalue = pcount + 1;
							pcount->num.valid++;
							pvalue->value = value[m];
							pvalue++;
						} 
						// insert value if they're not equal, 
						// or if we're not on a run and the run is less than 3 units
						else if ((value[m] != value[m-1]) 
							|| ((pcount->num.total == pcount->num.valid) && ((m < n - 1) && value[m] != value[m+1])))
						{
							if (pcount->num.total != pcount->num.valid)
							{
								//if (j == 0) printf("%d:%d   ", pcount->num.valid, pcount->num.total ); 
								pcount = pvalue;
								pvalue = pcount + 1;
							}
							pcount->num.valid++;
							pvalue->value = value[m];
							pvalue++;
						}
						pcount->num.total++;
					}
					//if (j == 0) printf("%d:%d\n", pcount->num.valid, pcount->num.total ); 

					panim->anim[w][j].num[k] = pvalue - data;
					if (panim->anim[w][j].num[k] == 2 && value[0] == 0)
					{
						panim->anim[w][j].num[k] = 0;
					}
					else
					{
						panim->anim[w][j].data[k] = (mstudioanimvalue_t *)calloc( pvalue - data, sizeof( mstudioanimvalue_t ) );
						memmove( panim->anim[w][j].data[k], data, (pvalue - data) * sizeof( mstudioanimvalue_t ) );
					}
					// printf("%d(%d) ", g_source[i]->panim[q]->numanim[j][k], n );
				}

				if (g_bCheckLengths)
				{
					char *tmp[6] = { "X", "Y", "Z", "XR", "YR", "ZR" };
					n = 0;
					float s = 0.0f;
					for (k = 0; k < 6; k++)
					{
						if (panim->anim[w][j].num[k])
						{
							if (n == 0)
								printf("%30s :", g_bonetable[j].name );
						
							// printf("%2s (%8.3f: %8.3f %8.3f) ", tmp[k], g_bonetable[j].pos[k], checkmin[k], checkmax[k] );
							if (k < 3)
								s = g_bonetable[j].posscale[k]; 
							else
								s = g_bonetable[j].rotscale[k-3]; 

							// printf("%2s %8.5f (%d %d)  ", tmp[k], checkmax[k] - checkmin[k] );
							printf("%2s %8.5f  ", tmp[k], (checkmax[k] - checkmin[k]) * s );
							n = 1;
						}
					}
					if (n)
						printf("\n");
				}
			}
		}

		if (panim->numsections == 1)
		{
			panim->sectionframes = 0;
		}
	}
}

//-----------------------------------------------------------------------------
// Compress a single animation stream
//-----------------------------------------------------------------------------


// this probably will brake quaternions, but idk
// not checked
static void CompressSingle( s_animationstream_t *pStream )
{
	int k, n, m;

	if (pStream->numerror == 0)
		return;

	// printf("%s : ", g_bonetable[j].name );
	for (k = 0; k < 6; k++)
	{
		float minv, maxv, scale;
		RadianEuler ang;

		if (k < 3) 
		{
			minv = -128.0;
			maxv = 128.0;
		}
		else
		{
			minv = -M_PI / 8.0;
			maxv = M_PI / 8.0;
		}

		for (n = 0; n < pStream->numerror; n++)
		{
			float v = 0.0f;
			switch(k)
			{
			case 0: 
			case 1: 
			case 2: 
				v = pStream->pError[n].pos[k];
				break;
			case 3:
			case 4:
			case 5:
				QuaternionAngles( pStream->pError[n].q, ang );
				v = ang[k-3];
				while (v >= M_PI)
					v -= M_PI * 2;
				while (v < -M_PI)
					v += M_PI * 2;
				break;
			}
			if (v < minv)
				minv = v;
			if (v > maxv)
				maxv = v;
		}
		// printf("%f %f\n", minv, maxv );
		if (minv < maxv)
		{
			if (-minv> maxv)
			{
				scale = minv / -32768.0;
			}
			else
			{
				scale = maxv / 32767;
			}
		}
		else
		{
			scale = 1.0 / 32.0;
		}

		pStream->scale[k] = scale;
		
		mstudioanimvalue_t	*pcount, *pvalue;
		float v;
		short value[MAXSTUDIOANIMFRAMES];
		mstudioanimvalue_t data[MAXSTUDIOANIMFRAMES];

		// find deltas from default pose
		for (n = 0; n < pStream->numerror; n++)
		{
			switch(k)
			{
			case 0: /* X Position */
			case 1: /* Y Position */
			case 2: /* Z Position */
				value[n] = pStream->pError[n].pos[k] / pStream->scale[k]; 
				break;
			case 3: /* X Rotation */
			case 4: /* Y Rotation */
			case 5: /* Z Rotation */
				QuaternionAngles( pStream->pError[n].q, ang );
				v = ang[k-3];
				while (v >= M_PI)
					v -= M_PI * 2;
				while (v < -M_PI)
					v += M_PI * 2;
				value[n] = v / pStream->scale[k];
				break;
			}
		}

		// initialize animation RLE block
		pStream->numanim[k] = 0;

		memset( data, 0, sizeof( data ) ); 
		pcount = data; 
		pvalue = pcount + 1;

		pcount->num.valid = 1;
		pcount->num.total = 1;
		pvalue->value = value[0];
		pvalue++;

		// build a RLE of deltas from the default pose
		for (m = 1; m < n; m++)
		{
			if (pcount->num.total == 255)
			{
				// chain too long, force a new entry
				pcount = pvalue;
				pvalue = pcount + 1;
				pcount->num.valid++;
				pvalue->value = value[m];
				pvalue++;
			} 
			// insert value if they're not equal, 
			// or if we're not on a run and the run is less than 3 units
			else if ((value[m] != value[m-1]) 
				|| ((pcount->num.total == pcount->num.valid) && ((m < n - 1) && value[m] != value[m+1])))
			{
				if (pcount->num.total != pcount->num.valid)
				{
					//if (j == 0) printf("%d:%d   ", pcount->num.valid, pcount->num.total ); 
					pcount = pvalue;
					pvalue = pcount + 1;
				}
				pcount->num.valid++;
				pvalue->value = value[m];
				pvalue++;
			}
			pcount->num.total++;
		}
		//if (j == 0) printf("%d:%d\n", pcount->num.valid, pcount->num.total ); 

		pStream->numanim[k] = pvalue - data;
		pStream->anim[k] = (mstudioanimvalue_t *)calloc( pvalue - data, sizeof( mstudioanimvalue_t ) );
		memmove( pStream->anim[k], data, (pvalue - data) * sizeof( mstudioanimvalue_t ) );
		// printf("%d (%d) : %d\n", pRule->numanim[k], n, pRule->errorData.numerror );
	}
}



//-----------------------------------------------------------------------------
// Compress all the IK data
//-----------------------------------------------------------------------------

static void CompressIKErrors( )
{
	int i, j;

	// find scales for all bones
	for (i = 0; i < g_numani; i++)
	{
		for (j = 0; j < g_panimation[i]->numikrules; j++)
		{
			s_ikrule_t *pRule = &g_panimation[i]->ikrule[j];

			if (pRule->errorData.numerror == 0)
				continue;

			CompressSingle( &pRule->errorData );
		}
	}
}

//-----------------------------------------------------------------------------
// Compress all the Local Hierarchy data
//-----------------------------------------------------------------------------

static void CompressLocalHierarchy( )
{
	int i, j;

	// find scales for all bones
	for (i = 0; i < g_numani; i++)
	{
		for (j = 0; j < g_panimation[i]->numlocalhierarchy; j++)
		{
			s_localhierarchy_t *pRule = &g_panimation[i]->localhierarchy[j];

			if (pRule->localData.numerror == 0)
				continue;

			CompressSingle( &pRule->localData );
		}
	}
}


//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
struct BonePriority_s
{
	int m_nGlobalBoneId;
	float m_nGlobalBoneWeight;
};


//-----------------------------------------------------------------------------
// Sort by bone weight
//-----------------------------------------------------------------------------
int compareBonePriority( const void *a, const void *b )
{
	return
		reinterpret_cast< const BonePriority_s * >( a )->m_nGlobalBoneWeight < reinterpret_cast< const BonePriority_s * >( b )->m_nGlobalBoneWeight ? -1 : 
		reinterpret_cast< const BonePriority_s * >( a )->m_nGlobalBoneWeight > reinterpret_cast< const BonePriority_s * >( b )->m_nGlobalBoneWeight ?  1 : 0;
};



//-----------------------------------------------------------------------------
// Dump A $definebone line, ensuring any parents are already dumped
//-----------------------------------------------------------------------------
void DumpDefineBone( int nBoneId, bool *pBoneDumpedList )
{
	Assert( nBoneId < g_numbones );

	if ( pBoneDumpedList[ nBoneId ] )
		return;

	const s_bonetable_t &bone = g_bonetable[ nBoneId ];

	// Ensure the parent bone is dumped before the child
	if ( bone.parent >= 0 )
	{
		DumpDefineBone( bone.parent, pBoneDumpedList );
	}

	printf( "$definebone " );

	printf( "\"%s\" ", bone.name );
	if ( bone.parent != -1 )
	{
		printf( "\"%s\" ", g_bonetable[ bone.parent ].name );
	}
	else
	{
		printf( "\"\" " );
	}

	Vector pos;
	QAngle angles;

	RadianEuler bone_rot;
	QuaternionAngles(bone.qrot, bone_rot);

	pos = bone.pos;
	angles.Init( RAD2DEG(bone_rot.y ), RAD2DEG(bone_rot.z ), RAD2DEG(bone_rot.x ) );
	printf( "%f %f %f %f %f %f", pos.x, pos.y, pos.z, angles.x, angles.y, angles.z );

	MatrixAngles( bone.srcRealign, angles, pos );
	printf( " %f %f %f %f %f %f", pos.x, pos.y, pos.z, angles.x, angles.y, angles.z );

	printf( "\n" );

	pBoneDumpedList[ nBoneId ] = true;
}


//-----------------------------------------------------------------------------
// Dump a $definebones .qci file with the bones in an optimal order
// i.e. Bones that are removed or replaced in LODs are later in the list
//      bones that are used all of the time are at the top of the list
//-----------------------------------------------------------------------------
void DumpDefineBones()
{
	BonePriority_s *pBonePriorityList = reinterpret_cast< BonePriority_s * >( stackalloc( g_numbones * sizeof( BonePriority_s ) ) );

	for ( int i = 0; i < g_numbones; ++i )
	{
		BonePriority_s &bonePriority = pBonePriorityList[ i ];
		bonePriority.m_nGlobalBoneId = i;
		bonePriority.m_nGlobalBoneWeight = 0.0f;
	}

	for ( int i = 0; i < g_ScriptLODs.Count(); ++i )
	{
		const LodScriptData_t &scriptLOD = g_ScriptLODs[ i ];

		for ( int j = 0; j < scriptLOD.boneReplacements.Count(); ++j )
		{
			// Ignore Shadow LOD
			if ( scriptLOD.switchValue <= 0.0f )
				continue;

			const int nBoneId = findGlobalBone( scriptLOD.boneReplacements[ j ].GetSrcName() );
			if ( nBoneId < 0 )
			{
				Warning( "Can't Find BoneReplacement Bone %s\n", scriptLOD.boneReplacements[ j ].GetSrcName() );
				continue;
			}

			pBonePriorityList[ nBoneId ].m_nGlobalBoneWeight += scriptLOD.switchValue;
		}
	}

	// bones used by hitboxes and attachments should always go first since they're used by the server
	for ( int i = 0; i < g_numbones; ++i )
	{
		if ( g_bonetable[i].flags & (BONE_USED_BY_HITBOX | BONE_USED_BY_ATTACHMENT | BONE_USED_BY_BONE_MERGE ))
		{
			pBonePriorityList[ i ].m_nGlobalBoneWeight = -1.0f;
		}
	}

	qsort( pBonePriorityList, g_numbones, sizeof( BonePriority_s ), compareBonePriority );

	bool *pBoneDumpedList = reinterpret_cast< bool * >( stackalloc( g_numbones * sizeof( bool ) ) );
	memset( pBoneDumpedList, 0, g_numbones * sizeof( bool ) );

	for (int i = 0; i < g_numbones; i++)
	{
		const BonePriority_s &bonePriority = pBonePriorityList[ i ];
		const int nBoneId = bonePriority.m_nGlobalBoneId;

		if (g_bonetable[ nBoneId ].flags & BONE_ALWAYS_PROCEDURAL)
		{
			pBoneDumpedList[ nBoneId ] = true;
			continue;
		}

		DumpDefineBone( nBoneId, pBoneDumpedList );
	}
}


void ReLinkAttachments()
{
	int	i;
	int	j;
	int	k;

	// relink per-model attachments, eyeballs
	for (i = 0; i < g_nummodelsbeforeLOD; i++)
	{
		s_source_t *psource = g_model[i]->source;
		for (j = 0; j < g_model[i]->numattachments; j++)
		{
			k = findGlobalBone( g_model[i]->attachment[j].bonename );
			if (k == -1)
			{
				MdlError("unknown model attachment link '%s'\n", g_model[i]->attachment[j].bonename );
			}
			g_model[i]->attachment[j].bone = j;
		}

		for (j = 0; j < g_model[i]->numeyeballs; j++)
		{
			g_model[i]->eyeball[j].bone = psource->boneLocalToGlobal[g_model[i]->eyeball[j].bone];
		}
	}
}

void CheckEyeballSetup()
{

	for (int i = 0; i < g_nummodelsbeforeLOD; i++)
	{
		for (int j = 0; j < g_model[i]->numeyeballs; j++)
		{
			s_eyeball_t *peyeball = &g_model[i]->eyeball[j];
			if (peyeball->upperlidflexdesc == -1)
			{
//				MdlWarning( "eyeball %s missing upperlid data\n", peyeball->name );

				int dummy = Add_Flexdesc( "dummy_eyelid" );

				peyeball->upperlidflexdesc = dummy;
				peyeball->upperflexdesc[0] = dummy;
				peyeball->uppertarget[0] = -1;
				peyeball->upperflexdesc[1] = dummy;
				peyeball->uppertarget[1] = 0;
				peyeball->upperflexdesc[2] = dummy;
				peyeball->uppertarget[2] = 1;
			}

			if (peyeball->lowerlidflexdesc == -1)
			{
//				MdlWarning( "eyeball %s missing lower data\n", peyeball->name );

				int dummy = Add_Flexdesc( "dummy_eyelid" );

				peyeball->lowerlidflexdesc = dummy;
				peyeball->lowerflexdesc[0] = dummy;
				peyeball->lowertarget[0] = -1;
				peyeball->lowerflexdesc[1] = dummy;
				peyeball->lowertarget[1] = 0;
				peyeball->lowerflexdesc[2] = dummy;
				peyeball->lowertarget[2] = 1;
			}
		}
	}


}

void SetupHitBoxes()
{
	int i;
	int	j;
	int	k;
	int	n;

	// set hitgroups
	for (k = 0; k < g_numbones; k++)
	{
		g_bonetable[k].group = -9999;
	}
	for (j = 0; j < g_numhitgroups; j++)
	{
		k = findGlobalBone( g_hitgroup[j].name );
		if (k != -1)
		{
			g_bonetable[k].group = g_hitgroup[j].group;
		}
		else
		{
			MdlError( "cannot find bone %s for hitgroup %d\n", g_hitgroup[j].name, g_hitgroup[j].group );
		}
	}
	for (k = 0; k < g_numbones; k++)
	{
		if (g_bonetable[k].group == -9999)
		{
			if (g_bonetable[k].parent != -1)
				g_bonetable[k].group = g_bonetable[g_bonetable[k].parent].group;
			else
				g_bonetable[k].group = 0;
		}
	}

	if ( g_hitboxsets.Count() == 0 )
	{
		int index = g_hitboxsets.AddToTail();

		s_hitboxset *set = &g_hitboxsets[ index ];
		memset( set, 0, sizeof( *set) );
		strcpy( set->hitboxsetname, "default" );

		gflags |= STUDIOHDR_FLAGS_AUTOGENERATED_HITBOX;

		// find intersection box volume for each bone
		for (k = 0; k < g_numbones; k++)
		{
			for (j = 0; j < 3; j++)
			{
				if (g_bUseBoneInBBox)
				{
					g_bonetable[k].bmin[j] = 0.0;
					g_bonetable[k].bmax[j] = 0.0;
				}
				else
				{
					g_bonetable[k].bmin[j] = 9999.0;
					g_bonetable[k].bmax[j] = -9999.0;
				}
			}
		}
		// try all the connect vertices
		for (i = 0; i < g_nummodelsbeforeLOD; i++)
		{
			s_loddata_t *pLodData = g_model[i]->m_pLodData;
			if ( !pLodData )
				continue;

			Vector	p;
			for (j = 0; j < pLodData->numvertices; j++)
			{
				for (n = 0; n < pLodData->vertex[j].boneweight.numbones; n++)
				{
					k = pLodData->vertex[j].boneweight.bone[n];
					VectorITransform( pLodData->vertex[j].position, g_bonetable[k].boneToPose, p );

					if (p[0] < g_bonetable[k].bmin[0]) g_bonetable[k].bmin[0] = p[0];
					if (p[1] < g_bonetable[k].bmin[1]) g_bonetable[k].bmin[1] = p[1];
					if (p[2] < g_bonetable[k].bmin[2]) g_bonetable[k].bmin[2] = p[2];
					if (p[0] > g_bonetable[k].bmax[0]) g_bonetable[k].bmax[0] = p[0];
					if (p[1] > g_bonetable[k].bmax[1]) g_bonetable[k].bmax[1] = p[1];
					if (p[2] > g_bonetable[k].bmax[2]) g_bonetable[k].bmax[2] = p[2];
				}
			}
		}
		// add in all your children as well
		for (k = 0; k < g_numbones; k++)
		{
			if ((j = g_bonetable[k].parent) != -1)
			{
				if (g_bonetable[k].pos[0] < g_bonetable[j].bmin[0]) g_bonetable[j].bmin[0] = g_bonetable[k].pos[0];
				if (g_bonetable[k].pos[1] < g_bonetable[j].bmin[1]) g_bonetable[j].bmin[1] = g_bonetable[k].pos[1];
				if (g_bonetable[k].pos[2] < g_bonetable[j].bmin[2]) g_bonetable[j].bmin[2] = g_bonetable[k].pos[2];
				if (g_bonetable[k].pos[0] > g_bonetable[j].bmax[0]) g_bonetable[j].bmax[0] = g_bonetable[k].pos[0];
				if (g_bonetable[k].pos[1] > g_bonetable[j].bmax[1]) g_bonetable[j].bmax[1] = g_bonetable[k].pos[1];
				if (g_bonetable[k].pos[2] > g_bonetable[j].bmax[2]) g_bonetable[j].bmax[2] = g_bonetable[k].pos[2];
			}
		}

		for (k = 0; k < g_numbones; k++)
		{
			if (g_bonetable[k].bmin[0] < g_bonetable[k].bmax[0] - 1
				&& g_bonetable[k].bmin[1] < g_bonetable[k].bmax[1] - 1
				&& g_bonetable[k].bmin[2] < g_bonetable[k].bmax[2] - 1)
			{
				set->hitbox[set->numhitboxes].bone = k;
				set->hitbox[set->numhitboxes].group = g_bonetable[k].group;
				VectorCopy( g_bonetable[k].bmin, set->hitbox[set->numhitboxes].bmin );
				VectorCopy( g_bonetable[k].bmax, set->hitbox[set->numhitboxes].bmax );

				if (dump_hboxes)
				{
					printf("$hbox %d \"%s\" %.2f %.2f %.2f  %.2f %.2f %.2f\n",
						set->hitbox[set->numhitboxes].group,
						g_bonetable[set->hitbox[set->numhitboxes].bone].name, 
						set->hitbox[set->numhitboxes].bmin[0], set->hitbox[set->numhitboxes].bmin[1], set->hitbox[set->numhitboxes].bmin[2],
						set->hitbox[set->numhitboxes].bmax[0], set->hitbox[set->numhitboxes].bmax[1], set->hitbox[set->numhitboxes].bmax[2] );

				}
				set->numhitboxes++;
			}
			else
			{
				// don't leave the invalid bounds in the table - future code will use it to compute sequence bounds for attachment points
				g_bonetable[k].bmin = vec3_origin;
				g_bonetable[k].bmax = vec3_origin;
			}
		}
	}
	else
	{
		gflags &= ~STUDIOHDR_FLAGS_AUTOGENERATED_HITBOX;

		for (int s = 0; s < g_hitboxsets.Count(); s++ )
		{
			s_hitboxset *set = &g_hitboxsets[ s ];

			for (j = 0; j < set->numhitboxes; j++)
			{
				k = findGlobalBone( set->hitbox[j].name );
				if (k != -1)
				{
					set->hitbox[j].bone = k;

#ifdef MDLCOMPILE
					// This is temporary
					// In mdlcompile, hitboxes come in defined in the space of the bone before remapping
					// i.e. In the space the bone was built by the user
					// In the near future, the hitboxes will be remapped before coming into studiomdl
					if ( g_bonetable[ k ].bPreAligned )
					{
						const matrix3x4_t &mSrcRealign = g_bonetable[ k ].srcRealign;
						Vector v = set->hitbox[j].bmin;
						VectorIRotate( v, mSrcRealign, set->hitbox[ j ].bmin );
						v = set->hitbox[j].bmax;
						VectorIRotate( v, mSrcRealign, set->hitbox[ j ].bmax );
					}

#endif // #ifdef MDLCOMPILE

				}
				else
				{
					MdlError( "cannot find bone %s for bbox\n", set->hitbox[j].name );
				}
			}
		}
	}

	for (int s = 0; s < g_hitboxsets.Count(); s++ )
	{
		s_hitboxset *set = &g_hitboxsets[ s ];

		// flag all bones used by hitboxes
		for (j = 0; j < set->numhitboxes; j++)
		{
			k = set->hitbox[j].bone;
			while (k != -1)
			{
				g_bonetable[k].flags |= BONE_USED_BY_HITBOX;
				k = g_bonetable[k].parent;
			}
		}
	}
}

void SetupFullBoneRenderBounds( CUtlVector<CBoneRenderBounds> &boneRenderBounds )
{
	boneRenderBounds.SetSize( g_numbones );


	// First, add the ones already calculated from vertices.
	for ( int i=0; i < g_numbones; i++ )
	{
		CBoneRenderBounds *pOut = &boneRenderBounds[i];
		pOut->m_Mins = g_bonetable[i].bmin;
		pOut->m_Maxs = g_bonetable[i].bmax;
	}

	// Note: shared animation files will need to include the hitboxes or else their sequence
	// boxes won't use this stuff.
	// Now add hitboxes.
	for ( int i=0; i < g_hitboxsets.Count(); i++ )
	{
		const s_hitboxset *pSet = &g_hitboxsets[i];
		
		for ( int k=0; k < pSet->numhitboxes; k++ )
		{
			const s_bbox_t *pIn = &pSet->hitbox[k];

			if ( pIn->bone >= 0 )
			{
				CBoneRenderBounds *pOut = &boneRenderBounds[pIn->bone];
				VectorMin( pIn->bmin, pOut->m_Mins, pOut->m_Mins );
				VectorMax( pIn->bmax, pOut->m_Maxs, pOut->m_Maxs );
			}
		}
	}
}


void CalcSequenceBoundingBoxes()
{
	int i;
	int	j;
	int	k;
	int	n;
	int	m;

	CUtlVector<CBoneRenderBounds> boneRenderBounds;
	SetupFullBoneRenderBounds( boneRenderBounds );

	// find bounding box for each g_sequence
	for (i = 0; i < g_numani; i++)
	{
		Vector bmin, bmax;
		
		// find intersection box volume for each bone
		for (j = 0; j < 3; j++)
		{
			bmin[j] = 9999.0;
			bmax[j] = -9999.0;
		}

		for (j = 0; j < g_panimation[i]->numframes; j++)
		{
			matrix3x4_t bonetransform[MAXSTUDIOBONES];	// bone transformation matrix
			matrix3x4_t posetransform[MAXSTUDIOBONES];	// bone transformation matrix
			matrix3x4_t bonematrix;						// local transformation matrix
			Vector pos;

			CalcBoneTransforms( g_panimation[i], j, bonetransform );

			for (k = 0; k < g_numbones; k++)
			{
				MatrixInvert( g_bonetable[k].boneToPose, bonematrix );
				ConcatTransforms (bonetransform[k], bonematrix, posetransform[k]);
			}

			// include hitboxes as well.
			if ( !g_bboxonlyverts )
			{
				for (k = 0; k < g_numbones; k++)
				{
					Vector tmpMin, tmpMax;
					TransformAABB( bonetransform[k], boneRenderBounds[k].m_Mins, boneRenderBounds[k].m_Maxs, tmpMin, tmpMax );
					VectorMin( tmpMin, bmin, bmin );
					VectorMax( tmpMax, bmax, bmax );

					if ( g_verbose &&
						(tmpMin.x < g_vecMinWorldspace.x ||
						 tmpMin.y < g_vecMinWorldspace.y ||
						 tmpMin.z < g_vecMinWorldspace.z ||
						 tmpMax.x > g_vecMaxWorldspace.x ||
						 tmpMax.y > g_vecMaxWorldspace.y ||
						 tmpMax.z > g_vecMaxWorldspace.z ) )
					{
						MdlWarning("%s : bone \"%s\" has bounding box out of range : %.0f %.0f %.0f : %.0f %.0f %.0f\n", 
							g_panimation[i]->name, g_bonetable[k].name,
							tmpMin.x, tmpMin.y, tmpMin.z, tmpMax.z, tmpMax.y, tmpMax.z );
					}
				}
			}

			// include vertices
			for (k = 0; k < g_nummodelsbeforeLOD; k++)
			{
				s_loddata_t *pLodData = g_model[k]->m_pLodData;

				// skip blank empty model
				if ( !pLodData )
					continue;

				for (n = 0; n < pLodData->numvertices; n++)
				{
					Vector tmp;
					pos = Vector( 0, 0, 0 );
					for (m = 0; m < pLodData->vertex[n].boneweight.numbones; m++)
					{
						VectorTransform( pLodData->vertex[n].position, posetransform[pLodData->vertex[n].boneweight.bone[m]], tmp ); // bug: should use all bones!
						VectorMA( pos, pLodData->vertex[n].boneweight.weight[m], tmp, pos );
					}

					VectorMin( pos, bmin, bmin );
					VectorMax( pos, bmax, bmax );
				}
			}
		}

		if (bmin.x < g_vecMinWorldspace.x || bmin.y < g_vecMinWorldspace.y || bmin.z < g_vecMinWorldspace.z || bmax.x > g_vecMaxWorldspace.x || bmax.y > g_vecMaxWorldspace.y || bmax.z > g_vecMaxWorldspace.z)
		{
			MdlWarning("%s : bounding box out of range : %.0f %.0f %.0f : %.0f %.0f %.0f\n", 
				g_panimation[i]->name, 
				bmin.x, bmin.y, bmin.z, bmax.z, bmax.y, bmax.z );

			VectorMax( bmin, g_vecMinWorldspace, bmin );
			VectorMin( bmax, g_vecMaxWorldspace, bmax );
		}

		VectorCopy( bmin, g_panimation[i]->bmin );
		VectorCopy( bmax, g_panimation[i]->bmax );

		/*
		printf("%s : %.0f %.0f %.0f %.0f %.0f %.0f\n", 
			g_panimation[i]->name, bmin[0], bmax[0], bmin[1], bmax[1], bmin[2], bmax[2] );
		*/

		// printf("%s  %.2f\n", g_sequence[i].name, g_sequence[i].panim[0]->pos[9][0][0] / g_bonetable[9].pos[0] );
	}

	for (i = 0; i < g_sequence.Count(); i++)
	{
		Vector bmin, bmax;
		
		// find intersection box volume for each bone
		for (j = 0; j < 3; j++)
		{
			bmin[j] = 9999.0;
			bmax[j] = -9999.0;
		}

		for (j = 0; j < g_sequence[i].groupsize[0]; j++)
		{
			for (k = 0; k < g_sequence[i].groupsize[1]; k++)
			{
				s_animation_t *panim = g_sequence[i].panim[j][k];

				if (panim->bmin[0] < bmin[0]) bmin[0] = panim->bmin[0];
				if (panim->bmin[1] < bmin[1]) bmin[1] = panim->bmin[1];
				if (panim->bmin[2] < bmin[2]) bmin[2] = panim->bmin[2];
				if (panim->bmax[0] > bmax[0]) bmax[0] = panim->bmax[0];
				if (panim->bmax[1] > bmax[1]) bmax[1] = panim->bmax[1];
				if (panim->bmax[2] > bmax[2]) bmax[2] = panim->bmax[2];
			}
		}

		VectorCopy( bmin, g_sequence[i].bmin );
		VectorCopy( bmax, g_sequence[i].bmax );
	}
}

void SetIlluminationPosition()
{
	// find center of domain
	if (!illumpositionset)
	{
		// Only use the 0th sequence; that should be the idle sequence
		VectorFill( illumposition, 0 );
		if (g_sequence.Count() != 0)
		{
			VectorAdd( g_sequence[0].bmin, g_sequence[0].bmax, illumposition );
			illumposition *= 0.5f;
		}
		illumpositionset = true;
	}
}

void SimplifyModel()
{
	if (g_sequence.Count() == 0 && g_numincludemodels == 0)
	{
		MdlError( "model has no sequences\n");
	}

	// have to load the lod sources before remapping bones so that the remap
	// happens for all LODs.
	LoadLODSources();

	RemapBones();

	LinkIKChains();

	LinkIKLocks();

	RealignBones();

	ConvertBoneTreeCollapsesToReplaceBones();

	// export bones
	if (g_definebones)
	{
		DumpDefineBones();
		exit( 0 );
	}

	// translate:
	// replacebone "bone0" "bone1"
	// replacebone "bone1" "bone2"
	// replacebone "bone2" "bone3"
	// to:
	// replacebone "bone0" "bone3"
	// replacebone "bone1" "bone3"
	// replacebone "bone2" "bone3"
	FixupReplacedBones();  

	RemapVerticesToGlobalBones();

	if (g_bCenterBonesOnVerts)
	{
		CenterBonesOnVerts();
	}
	
	// remap lods to root, building aggregate final pools
	// mark bones used by an lod
	UnifyLODs();
	
	if ( g_bPrintBones )
	{
		printf( "Hardware bone usage:\n" );
	}
	SpewBoneUsageStats();

	MarkParentBoneLODs();

	if ( g_bPrintBones )
	{
		printf( "CPU bone usage:\n" );
	}
	SpewBoneUsageStats();

	RemapAnimations();

	processAnimations();

	//limitBoneRotations();

	limitIKChainLength();

	RemapProceduralBones();

	MakeTransitions();
	RemapVertexAnimations();
	RemapVertexAnimationsNewVersion();

	FindAutolayers();

	// link bonecontrollers
	LinkBoneControllers();

	// link screen aligned bones
	TagScreenAlignedBones();

	TagWorldAlignedBones();

	// link attachments
	LinkAttachments();

	// link mouths
	LinkMouths();

	// procedural bone needs to propogate its bone usage up its chain
	// ensures runtime sets up dependent bone hierarchy
	MarkProceduralBoneChain();

	LockBoneLengths();

	ProcessIKRules();

	CompressIKErrors( );

	CompressLocalHierarchy( );

	CalcPoseParameters();

	ReLinkAttachments();

	CheckEyeballSetup();

	SetupHitBoxes();

	CompressAnimations( );

	CalcSequenceBoundingBoxes();

	SetIlluminationPosition();

	if ( g_bBuildPreview )
	{
		gflags |= STUDIOHDR_FLAGS_BUILT_IN_PREVIEW_MODE;
	}
}





