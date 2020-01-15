#ifndef PATCH_TRACER
#define PATCH_TRACER


#include "vert_field_graph.h"
#include "graph_query.h"
#include <vcg/complex/algorithms/voronoi_processing.h>
#include <vcg/math/matrix33.h>
#include <vcg/complex/algorithms/parametrization/tangent_field_operators.h>
#include <vcg/complex/algorithms/point_sampling.h>
#include <vcg/space/index/grid_static_ptr.h>
#include <vcg/complex/algorithms/closest.h>
#include <vcg/complex/algorithms/symmetry.h>
#include <wrap/io_trimesh/export_ply.h>
#include <wrap/io_trimesh/export_obj.h>

#define CONVEX_THR 5.0
#define CONCAVE_THR 5.0
#define NARROW_THR 5.0

#define MAX_SAMPLES 1000
#define MAX_NARROW_CONST 0.05

enum TypeVert{Narrow,Concave,Convex,Flat,Internal,Choosen,None};
enum TraceType{TraceDirect,DijkstraReceivers,TraceLoop};
enum PatchType{LowCorners,HighCorners,NonDisk,HasEmitter,MoreSing,IsOK};

template <class MeshType>
class PatchTracer
{
    typedef typename MeshType::CoordType CoordType;
    typedef typename MeshType::ScalarType ScalarType;
    typedef typename MeshType::FaceType FaceType;
    typedef typename MeshType::VertexType VertexType;

    VertexFieldGraph<MeshType> &VFGraph;

public:
    //vertices types and ortho directions, used to select sources
    ScalarType Drift;

private:

    std::vector<TypeVert> VertType;
    std::vector<std::vector<CoordType> > VertFlatDir;
    std::vector<std::vector<CoordType> > VertOrthoDir;

    std::vector<TypeVert > NodeEmitterTypes;
    std::vector<TypeVert> NodeReceiverTypes;

    ScalarType MaxNarrowWeight;

    void InitOrthoDirections()
    {
        VertOrthoDir.clear();
        VertOrthoDir.resize(Mesh().vert.size());
        VertFlatDir.clear();
        VertFlatDir.resize(Mesh().vert.size());
        for (size_t i=0;i<Mesh().face.size();i++)
            for (size_t j=0;j<3;j++)
            {
                if(!vcg::face::IsBorder(Mesh().face[i],j))continue;

                size_t IndexV0=vcg::tri::Index(Mesh(),Mesh().face[i].cV0(j));
                size_t IndexV1=vcg::tri::Index(Mesh(),Mesh().face[i].cV1(j));
                CoordType P0=Mesh().vert[IndexV0].P();
                CoordType P1=Mesh().vert[IndexV1].P();
                CoordType Dir=P1-P0;
                Dir.Normalize();

                CoordType OrthoDir=Dir;
                OrthoDir=Mesh().face[i].cN()^OrthoDir;

                vcg::Matrix33<ScalarType> Rot0=vcg::RotationMatrix(Mesh().face[i].cN(),Mesh().vert[IndexV0].N());
                vcg::Matrix33<ScalarType> Rot1=vcg::RotationMatrix(Mesh().face[i].cN(),Mesh().vert[IndexV1].N());

                CoordType OrthoDir0=Rot0*OrthoDir;
                CoordType OrthoDir1=Rot1*OrthoDir;

                OrthoDir0.Normalize();
                OrthoDir1.Normalize();

                VertOrthoDir[IndexV0].push_back(OrthoDir0);
                VertOrthoDir[IndexV1].push_back(OrthoDir1);
                VertFlatDir[IndexV0].push_back(-Dir);
                VertFlatDir[IndexV1].push_back(Dir);
            }
    }

    void FindConvexV(std::vector<size_t> &ConvexV)
    {
        //find the convex on the original border
        ConvexV.clear();
        vcg::tri::UpdateSelection<MeshType>::VertexCornerBorder(Mesh(),M_PI-M_PI/CONVEX_THR);
        for (size_t i=0;i<Mesh().vert.size();i++)
        {
            if (!Mesh().vert[i].IsS())continue;
            ConvexV.push_back(i);
        }
    }

    void FindConcaveV(std::vector<size_t> &ConcaveV)
    {
        //then find the concaves
        vcg::tri::UpdateSelection<MeshType>::VertexCornerBorder(Mesh(),M_PI+M_PI/CONCAVE_THR);
        for (size_t i=0;i<Mesh().vert.size();i++)
        {
            if (!Mesh().vert[i].IsB())continue;
            if (Mesh().vert[i].IsS())continue;
            ConcaveV.push_back(i);
        }
        std::sort(ConcaveV.begin(),ConcaveV.end());
        auto last=std::unique(ConcaveV.begin(),ConcaveV.end());
        ConcaveV.erase(last, ConcaveV.end());
    }

    void FindNarrowV(std::vector<size_t> &NarrowV)
    {
        //then find the concaves
        vcg::tri::UpdateSelection<MeshType>::VertexCornerBorder(Mesh(),2*M_PI-M_PI/NARROW_THR);
        for (size_t i=0;i<Mesh().vert.size();i++)
        {
            if (!Mesh().vert[i].IsB())continue;
            if (Mesh().vert[i].IsS())continue;
            NarrowV.push_back(i);
        }
        std::sort(NarrowV.begin(),NarrowV.end());
        auto last=std::unique(NarrowV.begin(),NarrowV.end());
        NarrowV.erase(last, NarrowV.end());
    }

    void FindFlatV(const std::vector<size_t> &ConvexV,
                   const std::vector<size_t> &ConcaveV,
                   const std::vector<size_t> &NarrowV,
                   std::vector<size_t> &FlatV)
    {
        FlatV.clear();

        vcg::tri::UpdateFlags<MeshType>::VertexClearV(Mesh());
        for (size_t i=0;i<ConvexV.size();i++)
            Mesh().vert[ConvexV[i]].SetV();

        for (size_t i=0;i<ConcaveV.size();i++)
            Mesh().vert[ConcaveV[i]].SetV();

        for (size_t i=0;i<NarrowV.size();i++)
            Mesh().vert[NarrowV[i]].SetV();

        for (size_t i=0;i<Mesh().face.size();i++)
            for (size_t j=0;j<3;j++)
            {
                bool IsB=vcg::face::IsBorder(Mesh().face[i],j);
                //bool IsSharp=Mesh().face[i].IsFaceEdgeS(j);
                if (!IsB)continue;

                VertexType *v0=Mesh().face[i].cV0(j);
                VertexType *v1=Mesh().face[i].cV1(j);
                size_t IndexV0=vcg::tri::Index(Mesh(),v0);
                size_t IndexV1=vcg::tri::Index(Mesh(),v1);

                if (!v0->IsV())FlatV.push_back(IndexV0);
                if (!v1->IsV())FlatV.push_back(IndexV1);
            }
        std::sort(FlatV.begin(),FlatV.end());
        auto last=std::unique(FlatV.begin(),FlatV.end());
        FlatV.erase(last, FlatV.end());
    }

    void InitVertType(const std::vector<size_t> &ConvexV,
                      const std::vector<size_t> &ConcaveV,
                      const std::vector<size_t> &NarrowV,
                      const std::vector<size_t> &FlatV)
    {

        VertType.clear();
        VertType.resize(Mesh().vert.size(),Internal);

        for (size_t i=0;i<ConvexV.size();i++)
        {
            size_t IndexV=ConvexV[i];
            VertType[IndexV]=Convex;
        }

        for (size_t i=0;i<ConcaveV.size();i++)
        {
            size_t IndexV=ConcaveV[i];
            VertType[IndexV]=Concave;
        }

        for (size_t i=0;i<NarrowV.size();i++)
        {
            size_t IndexV=NarrowV[i];
            VertType[IndexV]=Narrow;
        }

        for (size_t i=0;i<FlatV.size();i++)
        {
            size_t IndexV=FlatV[i];
            VertType[IndexV]=Flat;
        }
    }

    void InitVertType()
    {
        std::vector<size_t> ConvexV,ConcaveV,FlatV,NarrowV;
        FindConvexV(ConvexV);
        FindConcaveV(ConcaveV);
        FindNarrowV(NarrowV);
        FindFlatV(ConvexV,ConcaveV,NarrowV,FlatV);
        InitVertType(ConvexV,ConcaveV,NarrowV,FlatV);
    }

    void InitEmitters()
    {
        NodeEmitterTypes.clear();
        NodeReceiverTypes.clear();
        NodeEmitterTypes.resize(VFGraph.NumNodes(),None);
        NodeReceiverTypes.resize(VFGraph.NumNodes(),None);

        for (size_t i=0;i<Mesh().vert.size();i++)
        {

            if (VertType[i]==Convex)//convex ones, no emitters
                continue;

            if (VertType[i]==Internal)//this will be with sampling
                continue;

            if (VertType[i]==Flat)
            {
                assert(VertOrthoDir[i].size()==2);
                CoordType Ortho0=VertOrthoDir[i][0];
                CoordType Ortho1=VertOrthoDir[i][1];
                CoordType TargetD=Ortho0+Ortho1;
                TargetD.Normalize();
                size_t BestDir=VFGraph.GetClosestDirTo(i,TargetD);
                int IndexNode=VertexFieldGraph<MeshType>::IndexNode(i,BestDir);

                assert(NodeEmitterTypes[IndexNode]==None);
                NodeEmitterTypes[IndexNode]=Flat;

                //                //if real border then add receiver
                //                if (VFGraph.IsRealBorderVert(i))
                //                {
                size_t OppN=VertexFieldGraph<MeshType>::TangentNode(IndexNode);
                assert(NodeReceiverTypes[OppN]==None);
                NodeReceiverTypes[OppN]=Flat;
            }
            if (VertType[i]==Concave)
            {
                assert(VertOrthoDir[i].size()==2);
                CoordType Ortho0=VertOrthoDir[i][0];
                CoordType Ortho1=VertOrthoDir[i][1];
                size_t BestDir0=VFGraph.GetClosestDirTo(i,Ortho0);
                size_t BestDir1=VFGraph.GetClosestDirTo(i,Ortho1);

                size_t IndexNode0=VertexFieldGraph<MeshType>::IndexNode(i,BestDir0);
                size_t IndexNode1=VertexFieldGraph<MeshType>::IndexNode(i,BestDir1);

                assert(NodeEmitterTypes[IndexNode0]==None);
                NodeEmitterTypes[IndexNode0]=Concave;

                size_t OppN=VertexFieldGraph<MeshType>::TangentNode(IndexNode0);
                assert(NodeReceiverTypes[OppN]==None);
                NodeReceiverTypes[OppN]=Concave;

                if (IndexNode0!=IndexNode1)
                {
                    assert(NodeEmitterTypes[IndexNode1]==None);
                    NodeEmitterTypes[IndexNode1]=Concave;
                    OppN=VertexFieldGraph<MeshType>::TangentNode(IndexNode1);
                    assert(NodeReceiverTypes[OppN]==None);
                    NodeReceiverTypes[OppN]=Concave;
                }
            }

            if (VertType[i]==Narrow)
            {
                assert(VertFlatDir[i].size()==2);
                CoordType Flat0=VertFlatDir[i][0];
                CoordType Flat1=VertFlatDir[i][1];
                CoordType TargetD=Flat0+Flat1;
                TargetD.Normalize();
                size_t BestDir=VFGraph.GetClosestDirTo(i,TargetD);
                int IndexNode=VertexFieldGraph<MeshType>::IndexNode(i,BestDir);

                assert(NodeEmitterTypes[IndexNode]==None);
                NodeEmitterTypes[IndexNode]=Narrow;

                size_t OppN=VertexFieldGraph<MeshType>::TangentNode(IndexNode);
                assert(NodeReceiverTypes[OppN]==None);
                NodeReceiverTypes[OppN]=Narrow;
            }
        }

        //then add internal one for loops and other tracing
        std::vector<size_t> StartingNodes;
        if(sample_ratio<1)
        {
            size_t sampleNum=floor(sqrt(Mesh().vert.size())+0.5)*10*sample_ratio;
            sampleNum=std::max(sampleNum,(size_t)50);
            SampleStartingNodes(false,sampleNum,StartingNodes);
        }
        else
        {
            for (size_t i=0;i<Mesh().vert.size();i++)
            {
                std::vector<size_t> IndexN;
                VertexFieldGraph<MeshType>::IndexNodes(i,IndexN);
                if(VFGraph.IsActive(IndexN[0]))
                    StartingNodes.push_back(IndexN[0]);
                if(VFGraph.IsActive(IndexN[1]))
                    StartingNodes.push_back(IndexN[1]);
            }
        }
        for (size_t i=0;i<StartingNodes.size();i++)
        {
            size_t IndexV=VertexFieldGraph<MeshType>::NodeVertI(StartingNodes[i]);
            //std::cout<<"Sampled V "<<IndexV<<std::endl;
            if (VertType[IndexV]!=Internal)continue;
            assert(NodeEmitterTypes[StartingNodes[i]]==None);
            NodeEmitterTypes[StartingNodes[i]]=Internal;
            //Emitter[IndexV].push_back(StartingNodes[i]);
        }

        //        OriginalEmitter=Emitter;
        //        OriginalReceiver=Receiver;
    }

    void GetEdgeDir(const size_t &IndexV0,
                    const size_t &IndexV1,
                    size_t &DirN0,
                    size_t &DirN1)
    {
        CoordType Dir=Mesh().vert[IndexV1].P()-
                Mesh().vert[IndexV0].P();
        Dir.Normalize();
        DirN0=VFGraph.GetClosestDirTo(IndexV0,Dir);
        DirN1=VFGraph.GetClosestDirTo(IndexV1,-Dir);
    }

    void InvalidateTangentNodes()
    {
        VFGraph.SetAllActive();
        //TangentConstraints.clear();
        for (size_t i=0;i<VertType.size();i++)
        {
            if (VertType[i]==Convex)
            {
                std::vector<size_t> NodesI;
                VertexFieldGraph<MeshType>::IndexNodes(i,NodesI);
                for (size_t j=0;j<NodesI.size();j++)
                    VFGraph.SetActive(NodesI[j],false);
            }

            if (VertType[i]==Flat)//||(VertType[i]==Narrow))
            {
                //get the nodes of a given vertex
                std::vector<size_t> NodesI;
                VertexFieldGraph<MeshType>::IndexNodes(i,NodesI);
                for (size_t j=0;j<NodesI.size();j++)
                {
                    if (NodeEmitterTypes[NodesI[j]]!=None)continue;
                    if (NodeReceiverTypes[NodesI[j]]!=None)continue;
                    VFGraph.SetActive(NodesI[j],false);
                }
            }

            if (VertType[i]==Concave)
            {
                //get the nodes of a given vertex
                std::vector<size_t> NodesI;
                VertexFieldGraph<MeshType>::IndexNodes(i,NodesI);
                for (size_t j=0;j<NodesI.size();j++)
                {
                    if (NodeEmitterTypes[NodesI[j]]!=None)continue;
                    if (NodeReceiverTypes[NodesI[j]]!=None)continue;
                    VFGraph.SetActive(NodesI[j],false);
                }
            }
        }
    }

public:
    MeshType &Mesh()
    {
        return VFGraph.Mesh();
    }

private:

    bool GetSubSequence(size_t IndexN0,size_t IndexN1,
                        std::vector<size_t > &Sequence)
    {
        Sequence.clear();
        typename VertexFieldQuery<MeshType>::ShortParam SParam;
        SParam.StartNode.push_back(IndexN0);
        SParam.MaxAngle=-1;
        SParam.MaxTwin=-1;
        SParam.MaxWeight=-1;
        SParam.OnlyDirect=true;
        SParam.StopAtSel=false;
        SParam.TargetNode=IndexN1;
        SParam.DriftPenalty=Drift;
        SParam.LoopMode=false;
        SParam.MaxJump=-1;
        SParam.AvoidBorder=true;

        bool Traced=VertexFieldQuery<MeshType>::ShortestPath(VFGraph,SParam,Sequence);
        if (!Traced)return false;
        return true;
    }

    //used to expand a path
    bool ExpandPath(std::vector<size_t > &Path,bool IsLoop)
    {
        std::vector<size_t > SwapTraceNode;
        size_t Limit=Path.size()-1;
        if (IsLoop)Limit++;
        for (size_t i=0;i<Limit;i++)
        {
            size_t IndexN0=Path[i];
            size_t IndexN1=Path[(i+1)%Path.size()];
            std::vector<size_t> IndexN;
            bool found=GetSubSequence(IndexN0,IndexN1,IndexN);
            if(!found)return false;

            assert(IndexN.size()>=2);
            SwapTraceNode.insert(SwapTraceNode.end(),IndexN.begin(),IndexN.end()-1);
        }
        if (!IsLoop)
            SwapTraceNode.push_back(Path.back());

        Path=SwapTraceNode;
        return true;
    }

    void ExpandCandidates()
    {
        std::vector<CandidateTrace> ExpandedCandidates;

        size_t SelfIntN=0;
        for (size_t i=0;i<Candidates.size();i++)
        {
            bool expanded=ExpandPath(Candidates[i].PathNodes,Candidates[i].IsLoop);
            bool SelfInt=VertexFieldQuery<MeshType>::SelfIntersect(VFGraph,Candidates[i].PathNodes,Candidates[i].IsLoop);
            if (SelfInt){SelfIntN++;continue;}
            if (expanded)
                ExpandedCandidates.push_back(Candidates[i]);
        }
        std::cout<<"Self Intersections "<<SelfIntN<<std::endl;

        //all expanded do nothing (already chenged in place)
        if (ExpandedCandidates.size()==Candidates.size())return;
        Candidates=ExpandedCandidates;
    }

    void InitVerticesNeeds()
    {
        VerticesNeeds.resize(Mesh().vert.size(),0);
        for (size_t i=0;i<VertType.size();i++)
            if (VertType[i]==Narrow)VerticesNeeds[i]=1;
        for (size_t i=0;i<VertType.size();i++)
            if (VertType[i]==Concave)VerticesNeeds[i]=1;
    }

    void InitStructures()
    {
        InitOrthoDirections();

        InitVertType();

        InitEmitters();

        InvalidateTangentNodes();

        InitVerticesNeeds();
    }

    struct CandidateTrace
    {
        TypeVert FromType;
        TypeVert ToType;
        TraceType TracingMethod;
        size_t InitNode;
        std::vector<size_t> PathNodes;
        bool IsLoop;
        bool Updated;

        CandidateTrace(){}

        CandidateTrace(TypeVert _FromType,
                       TypeVert _ToType,
                       TraceType _TracingMethod,
                       size_t _InitNode)
        {
            FromType=_FromType;
            ToType=_ToType;
            TracingMethod=_TracingMethod;
            InitNode=_InitNode;
            IsLoop=false;
            Updated=false;
        }
    };

    std::vector<CandidateTrace> Candidates;
    std::vector<CandidateTrace> ChoosenPaths;

    std::vector<size_t> VerticesNeeds;
    //std::vector<size_t> ProblematicNodes;

    void CleanNonTracedCandidates()
    {
        std::vector<CandidateTrace> SwapCandidates;
        for (size_t i=0;i<Candidates.size();i++)
        {
            if ((Candidates[i].Updated) &&
                    (Candidates[i].PathNodes.size()==0))continue;
            SwapCandidates.push_back(Candidates[i]);
        }
        Candidates=SwapCandidates;
    }


    bool UpdateCandidates(const std::vector<bool> &CanReceive)
    {
        if (Candidates.size()==0)return false;

        assert(CanReceive.size()==VFGraph.NumNodes());
        VFGraph.ClearSelection();
        for (size_t i=0;i<CanReceive.size();i++)
        {
            if (CanReceive[i])
                VFGraph.Select(i);
        }

        for (size_t i=0;i<Candidates.size();i++)
        {
            if (Candidates[i].Updated)continue;

            Candidates[i].Updated=true;

            size_t IndexN0=Candidates[i].InitNode;
            assert(VFGraph.IsActive(IndexN0));
            if (Candidates[i].TracingMethod==TraceDirect)
            {
                std::vector<size_t> PathN;
                bool Traced=VertexFieldQuery<MeshType>::TraceToSelected(VFGraph,IndexN0,PathN);
                if (!Traced)continue;
                bool SelfInt=VertexFieldQuery<MeshType>::SelfIntersect(VFGraph,PathN,false);
                if (SelfInt)continue;
                Candidates[i].PathNodes=PathN;
                Candidates[i].IsLoop=false;
            }
            if (Candidates[i].TracingMethod==DijkstraReceivers)
            {
                std::vector<size_t> PathN;
                typename VertexFieldQuery<MeshType>::ShortParam SParam;
                SParam.StartNode.push_back(IndexN0);
                SParam.MaxTwin=1;
                SParam.MaxWeight=MaxNarrowWeight;

                SParam.OnlyDirect=false;
                SParam.DriftPenalty=Drift;
                bool Traced=VertexFieldQuery<MeshType>::ShortestPath(VFGraph,SParam,PathN);
                if (!Traced)continue;
                bool SelfInt=VertexFieldQuery<MeshType>::SelfIntersect(VFGraph,PathN,false);
                if (SelfInt)continue;
                Candidates[i].PathNodes=PathN;
                Candidates[i].IsLoop=false;
            }
            if (Candidates[i].TracingMethod==TraceLoop)
            {
                std::vector<size_t> PathN;
                bool ClosedLoop=VertexFieldQuery<MeshType>::FindLoop(VFGraph,IndexN0,PathN,Drift);
                if (!ClosedLoop)continue;
                bool SelfInt=VertexFieldQuery<MeshType>::SelfIntersect(VFGraph,PathN,true);
                if (SelfInt)continue;
                Candidates[i].PathNodes=PathN;
                Candidates[i].IsLoop=true;
            }
        }
        //finally erase the non traced ones
        CleanNonTracedCandidates();
        return (Candidates.size()>0);
    }

    bool CollideWithChoosen(const std::vector<size_t > &TestPath,
                            bool IsLoop,
                            size_t StartFrom=0)
    {
        for (size_t i=StartFrom;i<ChoosenPaths.size();i++)
        {
            bool collide=VertexFieldQuery<MeshType>::CollideTraces(VFGraph,TestPath,
                                                                   ChoosenPaths[i].PathNodes,
                                                                   IsLoop,ChoosenPaths[i].IsLoop);
            if (collide)return true;
        }
        return false;
    }

    std::vector<std::pair<ScalarType,size_t> > CandidatesPathLenghts;

    void InitCandidatesPathLenghts()
    {
        CandidatesPathLenghts.clear();
        for (size_t i=0;i<Candidates.size();i++)
        {
            ScalarType currL=VertexFieldQuery<MeshType>::TraceLenght(VFGraph,Candidates[i].PathNodes,
                                                                     Candidates[i].IsLoop);
            CandidatesPathLenghts.push_back(std::pair<ScalarType,size_t>(currL,i));
        }
        std::sort(CandidatesPathLenghts.begin(),CandidatesPathLenghts.end());
    }

    std::vector<std::pair<ScalarType,size_t> > CandidatesPathDistances;

    void InitCandidatesPathDistances()
    {
        CandidatesPathDistances.clear();
        for (size_t i=0;i<Candidates.size();i++)
        {
            ScalarType currD=VertexFieldQuery<MeshType>::TraceAVGDistance(VFGraph,Candidates[i].PathNodes);
            CandidatesPathDistances.push_back(std::pair<ScalarType,size_t>(currD,i));
        }
        std::sort(CandidatesPathDistances.begin(),CandidatesPathDistances.end());
        //should get the one with higher distance
        std::reverse(CandidatesPathDistances.begin(),CandidatesPathDistances.end());
    }

    void ChooseGreedyByLengthVertNeeds(bool UseVertNeeds=true,bool checkOnlylastConfl=false)
    {
        InitCandidatesPathLenghts();

        size_t StartConflPath=0;
        if (checkOnlylastConfl)
            StartConflPath=ChoosenPaths.size();

        for (size_t i=0;i<CandidatesPathLenghts.size();i++)
        {
            //get the current trace from the sorted ones
            size_t CurrTraceIndex=CandidatesPathLenghts[i].second;
            std::vector<size_t > CurrTrace=Candidates[CurrTraceIndex].PathNodes;
            bool IsCurrLoop=Candidates[CurrTraceIndex].IsLoop;

            //get the first vertex to check if it has been already traced or not
            size_t IndexN0=CurrTrace[0];
            size_t IndexN1=CurrTrace.back();

            size_t IndexV0=VertexFieldGraph<MeshType>::NodeVertI(IndexN0);
            size_t IndexV1=VertexFieldGraph<MeshType>::NodeVertI(IndexN1);

            //if it has already a trace then go on
            if ((UseVertNeeds)&&((VerticesNeeds[IndexV0]==0)&&(VerticesNeeds[IndexV1]==0)))continue;


            bool collide=CollideWithChoosen(CurrTrace,IsCurrLoop,StartConflPath);
            if (!collide)
            {
                ChoosenPaths.push_back(Candidates[CurrTraceIndex]);
                UpdateVertNeeds(CurrTrace);
            }
        }
    }

    void DeleteCandidates(const std::vector<bool> &To_Delete)
    {
        assert(To_Delete.size()==Candidates.size());
        std::vector<CandidateTrace> CandidatesSwap;
        for (size_t i=0;i<To_Delete.size();i++)
        {
            if (To_Delete[i])continue;
            CandidatesSwap.push_back(Candidates[i]);
        }
        Candidates=CandidatesSwap;
    }

    void UpdateVertNeeds(const std::vector<size_t> &TestTrace)
    {
        for (size_t i=0;i<TestTrace.size();i++)
        {
            size_t IndexN=TestTrace[i];
            size_t IndexV=VertexFieldGraph<MeshType>::NodeVertI(IndexN);

            //            TypeVert VType=VertType[IndexV];
            //            TypeVert NodeEType=NodeEmitterTypes[IndexN];
            //            TypeVert NodeRType=NodeReceiverTypes[IndexN];
            //            if ((VType==NodeEType)||(VType==NodeRType))
            if(VerticesNeeds[IndexV]>0)
                VerticesNeeds[IndexV]--;
        }
    }

    void UpdateVertNeedsFromChoosen()
    {
        for (size_t i=0;i<ChoosenPaths.size();i++)
            UpdateVertNeeds(ChoosenPaths[i].PathNodes);
    }

    bool ChooseNextByDistance(bool UseVertNeeds)
    {
        std::vector<bool> To_Delete(Candidates.size(),false);
        InitCandidatesPathDistances();

        for (size_t i=0;i<CandidatesPathDistances.size();i++)
        {

            //get the current trace from the sorted ones
            size_t CurrTraceIndex=CandidatesPathDistances[i].second;
            std::vector<size_t > CurrTrace=Candidates[CurrTraceIndex].PathNodes;
            bool IsCurrLoop=Candidates[CurrTraceIndex].IsLoop;


            if (UseVertNeeds)
            {
                assert(!IsCurrLoop);
                //get the first vertex to check if it has been already traced or not
                std::vector<size_t > CurrTrace=Candidates[CurrTraceIndex].PathNodes;
                size_t IndexN0=CurrTrace[0];
                size_t IndexN1=CurrTrace.back();

                size_t IndexV0=VertexFieldGraph<MeshType>::NodeVertI(IndexN0);
                size_t IndexV1=VertexFieldGraph<MeshType>::NodeVertI(IndexN1);

                if((VerticesNeeds[IndexV0]==0)&&(VerticesNeeds[IndexV1]==0))
                {
                    To_Delete[CurrTraceIndex]=true;
                    continue;
                }
            }

            //add if collide
            bool collide=CollideWithChoosen(CurrTrace,IsCurrLoop);
            if (collide)
            {
                To_Delete[CurrTraceIndex]=true;
                continue;
            }

            ChoosenPaths.push_back(Candidates[CurrTraceIndex]);

            UpdateVertNeeds(CurrTrace);

            UpdateDistancesWithLastChoosen();
            CurrNodeDist=VFGraph.Distances();
            To_Delete[CurrTraceIndex]=true;
            DeleteCandidates(To_Delete);
            return true;
        }
        DeleteCandidates(To_Delete);
        return false;
    }

    void ChooseGreedyByDistance(bool UseVertNeeds)
    {
        CurrNodeDist.clear();
        InitDistances();
        while (ChooseNextByDistance(UseVertNeeds));
    }


    void GetEmitterType(const TypeVert EmitType,std::vector<size_t> &NodeEmitType)
    {
        NodeEmitType.clear();
        for (size_t i=0;i<NodeEmitterTypes.size();i++)
            if (NodeEmitterTypes[i]==EmitType)
                NodeEmitType.push_back(i);
    }

    void GetUnsatisfiedEmitterType(const TypeVert EmitType,std::vector<size_t> &NodeEmitType)
    {
        assert((EmitType==Narrow)||(EmitType==Concave));
        std::vector<size_t> TempEmit;
        GetEmitterType(EmitType,TempEmit);

        NodeEmitType.clear();
        for (size_t i=0;i<TempEmit.size();i++)
        {
            size_t IndexV=VertexFieldGraph<MeshType>::NodeVertI(TempEmit[i]);
            if (VerticesNeeds[IndexV]==0)continue;
            NodeEmitType.push_back(TempEmit[i]);
        }
    }

    void GetReceiverType(const TypeVert ReceiveType,
                         std::vector<size_t> &NodeReceiveType)
    {
        NodeReceiveType.clear();
        for (size_t i=0;i<NodeReceiverTypes.size();i++)
            if (NodeReceiverTypes[i]==ReceiveType)
                NodeReceiveType.push_back(i);
    }

    void GetUnsatisfiedReceiverType(const TypeVert ReceiveType,
                                    std::vector<size_t> &NodeReceiveType)
    {
        assert((ReceiveType==Narrow)||(ReceiveType==Concave));
        std::vector<size_t> TempReceive;
        GetReceiverType(ReceiveType,TempReceive);

        NodeReceiveType.clear();
        for (size_t i=0;i<TempReceive.size();i++)
        {
            size_t IndexV=VertexFieldGraph<MeshType>::NodeVertI(TempReceive[i]);
            if (VerticesNeeds[IndexV]==0)continue;
            NodeReceiveType.push_back(TempReceive[i]);
        }
    }

    void GetChoosenNodes(std::vector<size_t> &ChoosenNodes)
    {
        ChoosenNodes.clear();
        for (size_t i=0;i<ChoosenPaths.size();i++)
            for (size_t j=0;j<ChoosenPaths[i].PathNodes.size();j++)
            {
                size_t IndexN=ChoosenPaths[i].PathNodes[j];
                ChoosenNodes.push_back(IndexN);
            }
    }

    void GetChoosenInclusiveEndNodes(std::vector<size_t> &EndInclusive)
    {
        EndInclusive.clear();
        for (size_t i=0;i<ChoosenPaths.size();i++)
        {
            size_t FirstNode=ChoosenPaths[i].PathNodes[0];
            size_t LastNode=ChoosenPaths[i].PathNodes.back();
            if (NodeReceiverTypes[FirstNode]==Choosen)
            {
                assert(!VFGraph.IsBorder(FirstNode));
                EndInclusive.push_back(FirstNode);
            }
            if (NodeReceiverTypes[LastNode]==Choosen)
            {
                assert(!VFGraph.IsBorder(LastNode));
                EndInclusive.push_back(VFGraph.TangentNode(LastNode));
            }
        }
    }

    void GetChoosenEndNodeEmitters(std::vector<size_t> &EndChoosenEmitters)
    {
        EndChoosenEmitters.clear();

        //get the inclusive end nodes
        std::vector<size_t> EndInclusive;
        GetChoosenInclusiveEndNodes(EndInclusive);
        std::set<size_t> EndInclusiveSet(EndInclusive.begin(),EndInclusive.end());

        //then invert them
        for (size_t i=0;i<EndInclusive.size();i++)
        {
            size_t testNode=EndInclusive[i];
            assert(!VFGraph.IsBorder(testNode));
            testNode=VFGraph.TangentNode(testNode);

            //sampled in both directions then exclude it
            if (EndInclusiveSet.count(testNode)==1)continue;

            EndChoosenEmitters.push_back(testNode);
        }
    }

    void GetChoosenEndNodeReceivers(std::vector<size_t> &EndChoosenReceivers)
    {
        EndChoosenReceivers.clear();

        //get the inclusive end nodes
        std::vector<size_t> EndInclusive;
        GetChoosenInclusiveEndNodes(EndInclusive);
        std::set<size_t> EndInclusiveSet(EndInclusive.begin(),EndInclusive.end());

        //then invert them
        for (size_t i=0;i<EndInclusive.size();i++)
        {
            size_t testNode=EndInclusive[i];
            assert(!VFGraph.IsBorder(testNode));

            //sampled in both directions then exclude it
            size_t invNode=VFGraph.TangentNode(testNode);
            if (EndInclusiveSet.count(invNode)==1)continue;

            EndChoosenReceivers.push_back(testNode);
        }
    }

    void GetChoosenNodesAndTangent(std::vector<size_t> &ChoosenNodes)
    {
        GetChoosenNodes(ChoosenNodes);
        std::vector<size_t> TangentNodes=ChoosenNodes;
        VertexFieldGraph<MeshType>::TangentNodes(TangentNodes);
        ChoosenNodes.insert(ChoosenNodes.end(),TangentNodes.begin(),TangentNodes.end());
    }

    void GetConvexNodes(std::vector<size_t> &ConvexNodes)
    {
        ConvexNodes.clear();
        for (size_t i=0;i<VertType.size();i++)
        {
            if (VertType[i]==Convex)
            {
                std::vector<size_t> NodesI;
                VertexFieldGraph<MeshType>::IndexNodes(i,NodesI);
                ConvexNodes.insert(ConvexNodes.end(),NodesI.begin(),NodesI.end());
            }
        }
    }

    void GetFlatTangentNodes(std::vector<size_t> &TangentBorderNodes)
    {
        TangentBorderNodes.clear();

        for (size_t i=0;i<VertType.size();i++)
        {
            if (VertType[i]==Flat)
            {
                //get the nodes of a given vertex
                std::vector<size_t> NodesI;
                VertexFieldGraph<MeshType>::IndexNodes(i,NodesI);
                for (size_t j=0;j<NodesI.size();j++)
                {
                    if (NodeEmitterTypes[NodesI[j]]!=None)continue;
                    if (NodeReceiverTypes[NodesI[j]]!=None)continue;
                    TangentBorderNodes.push_back(NodesI[j]);
                }
            }
        }
    }

    void GetConcaveNodes(std::vector<size_t> &ConcaveNodes)
    {
        ConcaveNodes.clear();

        for (size_t i=0;i<VertType.size();i++)
        {
            if (VertType[i]==Concave)
            {
                //get the nodes of a given vertex
                std::vector<size_t> NodesI;
                VertexFieldGraph<MeshType>::IndexNodes(i,NodesI);
                for (size_t j=0;j<NodesI.size();j++)
                    ConcaveNodes.push_back(NodesI[j]);
            }
        }
    }

    void GetNarrowNodes(std::vector<size_t> &NarrowNodes)
    {
        NarrowNodes.clear();

        for (size_t i=0;i<VertType.size();i++)
        {
            if (VertType[i]==Narrow)
            {
                //get the nodes of a given vertex
                std::vector<size_t> NodesI;
                VertexFieldGraph<MeshType>::IndexNodes(i,NodesI);
                for (size_t j=0;j<NodesI.size();j++)
                    NarrowNodes.push_back(NodesI[j]);
            }
        }
    }

    void GetNarrowEmitters(std::vector<size_t> &NarrowEmittersNodes)
    {
        NarrowEmittersNodes.clear();

        for (size_t i=0;i<NodeEmitterTypes.size();i++)
            if (NodeEmitterTypes[i]==Narrow)
                NarrowEmittersNodes.push_back(i);
    }

    void GetNarrowReceivers(std::vector<size_t> &NarrowReceiversNodes)
    {
        NarrowReceiversNodes.clear();

        for (size_t i=0;i<NodeReceiverTypes.size();i++)
            if (NodeReceiverTypes[i]==Narrow)
                NarrowReceiversNodes.push_back(i);
    }

public:

    void getCornerSharp(std::vector<size_t> &CornerSharp)
    {
        CornerSharp.clear();
        for (size_t i=0;i<VertType.size();i++)
        {
            if ((VertType[i]==Narrow)||
                    (VertType[i]==Concave)||
                    (VertType[i]==Convex))
                CornerSharp.push_back(i);
        }
    }

    void TestGetNodes(const TypeVert FromType,
                      const TypeVert ToType,
                      const TraceType TracingType,
                      std::vector<size_t> &Emitter,
                      std::vector<size_t> &Receiver,
                      std::vector<size_t> &Disabled)
    {
        Emitter.clear();
        Receiver.clear();
        Disabled.clear();
        std::vector<bool> CanEmit,CanReceive,MustDisable;
        GetConfiguration(FromType,ToType,TracingType,CanEmit,CanReceive,MustDisable);

        for (size_t i=0;i<CanEmit.size();i++)
            if (CanEmit[i])Emitter.push_back(i);
        for (size_t i=0;i<CanReceive.size();i++)
            if (CanReceive[i])Receiver.push_back(i);
        for (size_t i=0;i<MustDisable.size();i++)
            if (MustDisable[i])Disabled.push_back(i);
    }

    bool AllReceivers;

private:

    void GetConfiguration(const TypeVert FromType,
                          const TypeVert ToType,
                          const TraceType TracingType,
                          std::vector<bool> &CanEmit,
                          std::vector<bool> &CanReceive,
                          std::vector<bool> &MustDisable)
    {
        CanEmit.clear();
        CanReceive.clear();
        MustDisable.clear();
        CanEmit.resize(VFGraph.NumNodes(),false);
        CanReceive.resize(VFGraph.NumNodes(),false);
        MustDisable.resize(VFGraph.NumNodes(),false);

        //all convex must be disabled always
        std::vector<size_t> ConvexNodes;
        GetConvexNodes(ConvexNodes);

        //leave it valid only when not all receivers and trace to flat
        if ((!AllReceivers)||(ToType!=Flat))
        {
            for (size_t i=0;i<ConvexNodes.size();i++)
                MustDisable[ConvexNodes[i]]=true;
        }

        //by default also disable the traced ones
        std::vector<size_t> ChoosenAndTangentNodes;
        GetChoosenNodesAndTangent(ChoosenAndTangentNodes);
        for (size_t i=0;i<ChoosenAndTangentNodes.size();i++)
            MustDisable[ChoosenAndTangentNodes[i]]=true;


        //also disable the border ones
        std::vector<size_t> TangentBorderNodes;
        GetFlatTangentNodes(TangentBorderNodes);
        if ((!AllReceivers)||(ToType!=Flat))
        {
            for (size_t i=0;i<TangentBorderNodes.size();i++)
                MustDisable[TangentBorderNodes[i]]=true;
        }

        if ((FromType==Narrow)&&(ToType==Narrow))
        {
            assert(TracingType!=TraceLoop);

            //get concave nodes
            std::vector<size_t> ConcaveNodes;
            GetConcaveNodes(ConcaveNodes);

            //then disable them (cannot pass throught them)
            for (size_t i=0;i<ConcaveNodes.size();i++)
                MustDisable[ConcaveNodes[i]]=true;

            //set narrow emitter active and non satisfied
            std::vector<size_t> EmitterNodes;
            GetUnsatisfiedEmitterType(Narrow,EmitterNodes);
            for (size_t i=0;i<EmitterNodes.size();i++)
            {
                if (MustDisable[EmitterNodes[i]])continue;
                CanEmit[EmitterNodes[i]]=true;
            }

            //set narrow emitter receiver, active and non satisfied
            std::vector<size_t> ReceiverNodes;
            GetUnsatisfiedReceiverType(Narrow,ReceiverNodes);
            for (size_t i=0;i<ReceiverNodes.size();i++)
            {
                if (MustDisable[ReceiverNodes[i]])continue;
                CanReceive[ReceiverNodes[i]]=true;
            }

            return;
        }

        if ((FromType==Narrow)&&(ToType==Concave))
        {
            assert(TracingType!=TraceLoop);

            //narrow receivers
            std::vector<size_t> NarrowReceivers;
            GetReceiverType(Narrow,NarrowReceivers);
            for (size_t i=0;i<NarrowReceivers.size();i++)
                MustDisable[NarrowReceivers[i]]=true;

            //concave emitters
            std::vector<size_t> ConcaveEmitters;
            GetEmitterType(Concave,ConcaveEmitters);
            for (size_t i=0;i<ConcaveEmitters.size();i++)
                MustDisable[ConcaveEmitters[i]]=true;


            //set narrow emitter active and non satisfied
            std::vector<size_t> EmitterNodes;
            GetUnsatisfiedEmitterType(Narrow,EmitterNodes);
            for (size_t i=0;i<EmitterNodes.size();i++)
            {
                if (MustDisable[EmitterNodes[i]])continue;
                CanEmit[EmitterNodes[i]]=true;
            }

            //set narrow emitter receiver, active and non satisfied
            std::vector<size_t> ReceiverNodes;
            GetUnsatisfiedReceiverType(Concave,ReceiverNodes);
            for (size_t i=0;i<ReceiverNodes.size();i++)
            {
                if (MustDisable[ReceiverNodes[i]])continue;
                CanReceive[ReceiverNodes[i]]=true;
            }

            return;
        }

        if ((FromType==Narrow)&&(ToType==Flat))
        {
            assert(TracingType!=TraceLoop);

            //narrow receivers
            std::vector<size_t> NarrowReceivers;
            GetReceiverType(Narrow,NarrowReceivers);

            //concave emitters
            std::vector<size_t> ConcaveNodes;
            GetConcaveNodes(ConcaveNodes);

            //            if (!AllReceivers)
            //            {
            for (size_t i=0;i<NarrowReceivers.size();i++)
                MustDisable[NarrowReceivers[i]]=true;

            for (size_t i=0;i<ConcaveNodes.size();i++)
                MustDisable[ConcaveNodes[i]]=true;
            //           }

            //set narrow emitter active and non satisfied
            std::vector<size_t> EmitterNodes;
            GetUnsatisfiedEmitterType(Narrow,EmitterNodes);
            for (size_t i=0;i<EmitterNodes.size();i++)
            {
                if (MustDisable[EmitterNodes[i]])continue;
                CanEmit[EmitterNodes[i]]=true;
            }

            //set flat emitter receiver, active and non satisfied
            std::vector<size_t> ReceiverNodes;
            GetReceiverType(Flat,ReceiverNodes);
            for (size_t i=0;i<ReceiverNodes.size();i++)
            {
                if (MustDisable[ReceiverNodes[i]])continue;
                CanReceive[ReceiverNodes[i]]=true;
            }

            if (AllReceivers)
            {
                for (size_t i=0;i<ConvexNodes.size();i++)
                    CanReceive[ConvexNodes[i]]=true;
                for (size_t i=0;i<TangentBorderNodes.size();i++)
                    CanReceive[TangentBorderNodes[i]]=true;
            }
            return;
        }

        //        if ((FromType==Narrow)&&(ToType==Choosen))
        //        {
        //            assert(TracingType!=TraceLoop);

        //            //set narrow emitter active and non satisfied
        //            std::vector<size_t> EmitterNodes;
        //            GetUnsatisfiedEmitterType(Narrow,EmitterNodes);
        //            for (size_t i=0;i<EmitterNodes.size();i++)
        //            {
        //                if (MustDisable[EmitterNodes[i]])continue;
        //                CanEmit[EmitterNodes[i]]=true;
        //            }

        //            //narrow receivers
        //            std::vector<size_t> NarrowReceivers;
        //            GetReceiverType(Narrow,NarrowReceivers);

        //            //concave emitters
        //            std::vector<size_t> ConcaveNodes;
        //            GetConcaveNodes(ConcaveNodes);

        //            for (size_t i=0;i<NarrowReceivers.size();i++)
        //                MustDisable[NarrowReceivers[i]]=true;

        //            for (size_t i=0;i<ConcaveNodes.size();i++)
        //                MustDisable[ConcaveNodes[i]]=true;

        //            //set flat emitter receiver, active and non satisfied
        //            //TODO ENABLE FIRST AND LAST
        //            std::vector<size_t> ReceiverNodes;
        //            GetReceiverType(Choosen,ReceiverNodes);
        //            for (size_t i=0;i<ReceiverNodes.size();i++)
        //            {
        //                if (MustDisable[ReceiverNodes[i]])continue;
        //                CanReceive[ReceiverNodes[i]]=true;
        //            }

        //            //add end node receivers
        //            std::vector<size_t> EndChoosenReceivers;
        //            GetChoosenEndNodeReceivers(EndChoosenReceivers);
        //            for (size_t i=0;i<EndChoosenReceivers.size();i++)
        //            {
        //                MustDisable[EndChoosenReceivers[i]]=false;
        //                CanReceive[EndChoosenReceivers[i]]=true;
        //            }

        //            return;
        //        }

        if ((FromType==Concave)&&(ToType==Concave))
        {
            assert(TracingType!=TraceLoop);

            //narrow receivers
            std::vector<size_t> NarrowReceivers;
            GetReceiverType(Narrow,NarrowReceivers);
            for (size_t i=0;i<NarrowReceivers.size();i++)
                MustDisable[NarrowReceivers[i]]=true;

            //narrow emitters
            std::vector<size_t> NarrowEmitters;
            GetEmitterType(Narrow,NarrowEmitters);
            for (size_t i=0;i<NarrowEmitters.size();i++)
                MustDisable[NarrowEmitters[i]]=true;


            //set concave emitter active and non satisfied
            std::vector<size_t> EmitterNodes;
            GetUnsatisfiedEmitterType(Concave,EmitterNodes);
            for (size_t i=0;i<EmitterNodes.size();i++)
            {
                if (MustDisable[EmitterNodes[i]])continue;
                CanEmit[EmitterNodes[i]]=true;
            }

            //set narrow emitter receiver, active and non satisfied
            std::vector<size_t> ReceiverNodes;
            GetUnsatisfiedReceiverType(Concave,ReceiverNodes);
            for (size_t i=0;i<ReceiverNodes.size();i++)
            {
                if (MustDisable[ReceiverNodes[i]])continue;
                CanReceive[ReceiverNodes[i]]=true;
            }
            return;
        }

        if ((FromType==Concave)&&(ToType==Flat))
        {
            assert(TracingType!=TraceLoop);

            //narrow receivers
            std::vector<size_t> NarrowReceivers;
            GetReceiverType(Narrow,NarrowReceivers);
            for (size_t i=0;i<NarrowReceivers.size();i++)
                MustDisable[NarrowReceivers[i]]=true;

            //narrow emitters
            std::vector<size_t> NarrowEmitters;
            GetEmitterType(Narrow,NarrowEmitters);
            for (size_t i=0;i<NarrowEmitters.size();i++)
                MustDisable[NarrowEmitters[i]]=true;

            //concave receivers
            std::vector<size_t> ConcaveReceivers;
            GetReceiverType(Concave,ConcaveReceivers);
            for (size_t i=0;i<ConcaveReceivers.size();i++)
                MustDisable[ConcaveReceivers[i]]=true;


            //set concave emitter active and non satisfied
            std::vector<size_t> EmitterNodes;
            GetUnsatisfiedEmitterType(Concave,EmitterNodes);
            for (size_t i=0;i<EmitterNodes.size();i++)
            {
                if (MustDisable[EmitterNodes[i]])continue;
                CanEmit[EmitterNodes[i]]=true;
            }

            //set flat receiver, active and non satisfied
            std::vector<size_t> ReceiverNodes;
            GetReceiverType(Flat,ReceiverNodes);
            for (size_t i=0;i<ReceiverNodes.size();i++)
            {
                if (MustDisable[ReceiverNodes[i]])continue;
                CanReceive[ReceiverNodes[i]]=true;
            }
            if (AllReceivers)
            {
                for (size_t i=0;i<ConvexNodes.size();i++)
                    CanReceive[ConvexNodes[i]]=true;

                for (size_t i=0;i<TangentBorderNodes.size();i++)
                    CanReceive[TangentBorderNodes[i]]=true;
            }
            return;
        }

        //        if ((FromType==Concave)&&(ToType==Choosen))
        //        {
        //            assert(TracingType!=TraceLoop);

        //            //narrow receivers
        //            std::vector<size_t> NarrowReceivers;
        //            GetReceiverType(Narrow,NarrowReceivers);
        //            for (size_t i=0;i<NarrowReceivers.size();i++)
        //                MustDisable[NarrowReceivers[i]]=true;

        //            //narrow emitters
        //            std::vector<size_t> NarrowEmitters;
        //            GetEmitterType(Narrow,NarrowEmitters);
        //            for (size_t i=0;i<NarrowEmitters.size();i++)
        //                MustDisable[NarrowEmitters[i]]=true;

        //            //concave receivers
        //            std::vector<size_t> ConcaveReceivers;
        //            GetReceiverType(Concave,ConcaveReceivers);
        //            for (size_t i=0;i<ConcaveReceivers.size();i++)
        //                MustDisable[ConcaveReceivers[i]]=true;

        //            //set concave emitter active and non satisfied
        //            std::vector<size_t> EmitterNodes;
        //            GetUnsatisfiedEmitterType(Concave,EmitterNodes);
        //            for (size_t i=0;i<EmitterNodes.size();i++)
        //            {
        //                if (MustDisable[EmitterNodes[i]])continue;
        //                CanEmit[EmitterNodes[i]]=true;
        //            }

        //            //set flat emitter receiver, active and non satisfied
        //            //TODO ENABLE FIRST AND LAST
        //            std::vector<size_t> ReceiverNodes;
        //            GetReceiverType(Choosen,ReceiverNodes);
        //            for (size_t i=0;i<ReceiverNodes.size();i++)
        //            {
        //                if (MustDisable[ReceiverNodes[i]])continue;
        //                CanReceive[ReceiverNodes[i]]=true;
        //            }

        //            //add end node receivers
        //            std::vector<size_t> EndChoosenReceivers;
        //            GetChoosenEndNodeReceivers(EndChoosenReceivers);
        //            for (size_t i=0;i<EndChoosenReceivers.size();i++)
        //            {
        //                MustDisable[EndChoosenReceivers[i]]=false;
        //                CanReceive[EndChoosenReceivers[i]]=true;
        //            }

        //            return;
        //        }

        if ((FromType==Flat)&&(ToType==Flat))
        {
            //narrow receivers
            std::vector<size_t> NarrowReceivers;
            GetReceiverType(Narrow,NarrowReceivers);
            for (size_t i=0;i<NarrowReceivers.size();i++)
                MustDisable[NarrowReceivers[i]]=true;

            //narrow emitters
            std::vector<size_t> NarrowEmitters;
            GetEmitterType(Narrow,NarrowEmitters);
            for (size_t i=0;i<NarrowEmitters.size();i++)
                MustDisable[NarrowEmitters[i]]=true;

            //narrow emitters
            std::vector<size_t> ConcaveNodes;
            GetConcaveNodes(ConcaveNodes);
            for (size_t i=0;i<ConcaveNodes.size();i++)
                MustDisable[ConcaveNodes[i]]=true;

            //set concave emitter active and non satisfied
            std::vector<size_t> EmitterNodes;
            GetEmitterType(Flat,EmitterNodes);
            for (size_t i=0;i<EmitterNodes.size();i++)
            {
                if (MustDisable[EmitterNodes[i]])continue;
                CanEmit[EmitterNodes[i]]=true;
            }

            std::vector<size_t> ReceiverNodes;
            GetReceiverType(Flat,ReceiverNodes);
            for (size_t i=0;i<ReceiverNodes.size();i++)
            {
                if (MustDisable[ReceiverNodes[i]])continue;
                CanReceive[ReceiverNodes[i]]=true;
            }
            return;
        }

        //        if ((FromType==Flat)&&(ToType==Choosen))
        //        {
        //            //narrow receivers
        //            std::vector<size_t> NarrowReceivers;
        //            GetReceiverType(Narrow,NarrowReceivers);
        //            for (size_t i=0;i<NarrowReceivers.size();i++)
        //                MustDisable[NarrowReceivers[i]]=true;

        //            //narrow emitters
        //            std::vector<size_t> NarrowEmitters;
        //            GetEmitterType(Narrow,NarrowEmitters);
        //            for (size_t i=0;i<NarrowEmitters.size();i++)
        //                MustDisable[NarrowEmitters[i]]=true;

        //            //narrow emitters
        //            std::vector<size_t> ConcaveNodes;
        //            GetConcaveNodes(ConcaveNodes);
        //            for (size_t i=0;i<ConcaveNodes.size();i++)
        //                MustDisable[ConcaveNodes[i]]=true;

        //            //set concave emitter active and non satisfied
        //            std::vector<size_t> EmitterNodes;
        //            GetEmitterType(Flat,EmitterNodes);
        //            for (size_t i=0;i<EmitterNodes.size();i++)
        //            {
        //                if (MustDisable[EmitterNodes[i]])continue;
        //                CanEmit[EmitterNodes[i]]=true;
        //            }

        //            //set flat emitter receiver, active and non satisfied
        //            //TODO ENABLE FIRST AND LAST
        //            std::vector<size_t> ReceiverNodes;
        //            GetReceiverType(Choosen,ReceiverNodes);
        //            for (size_t i=0;i<ReceiverNodes.size();i++)
        //            {
        //                if (MustDisable[ReceiverNodes[i]])continue;
        //                CanReceive[ReceiverNodes[i]]=true;
        //            }

        //            //add end node receivers
        //            std::vector<size_t> EndChoosenReceivers;
        //            GetChoosenEndNodeReceivers(EndChoosenReceivers);
        //            for (size_t i=0;i<EndChoosenReceivers.size();i++)
        //            {
        //                MustDisable[EndChoosenReceivers[i]]=false;
        //                CanReceive[EndChoosenReceivers[i]]=true;
        //            }

        //            return;
        //        }

        //        if ((FromType==Choosen)&&(ToType==Choosen))
        //        {
        //            //narrow receivers
        //            std::vector<size_t> NarrowReceivers;
        //            GetReceiverType(Narrow,NarrowReceivers);
        //            for (size_t i=0;i<NarrowReceivers.size();i++)
        //                MustDisable[NarrowReceivers[i]]=true;

        //            //narrow emitters
        //            std::vector<size_t> NarrowEmitters;
        //            GetEmitterType(Narrow,NarrowEmitters);
        //            for (size_t i=0;i<NarrowEmitters.size();i++)
        //                MustDisable[NarrowEmitters[i]]=true;

        //            //narrow emitters
        //            std::vector<size_t> ConcaveNodes;
        //            GetConcaveNodes(ConcaveNodes);
        //            for (size_t i=0;i<ConcaveNodes.size();i++)
        //                MustDisable[ConcaveNodes[i]]=true;

        //            //set flat emitter receiver, active and non satisfied
        //            //TODO ENABLE FIRST AND LAST
        //            std::vector<size_t> EmitterNodes;
        //            GetReceiverType(Choosen,EmitterNodes);
        //            for (size_t i=0;i<EmitterNodes.size();i++)
        //            {
        //                if (MustDisable[EmitterNodes[i]])continue;
        //                CanEmit[EmitterNodes[i]]=true;
        //            }

        //            std::vector<size_t> ReceiverNodes;
        //            GetReceiverType(Choosen,ReceiverNodes);
        //            for (size_t i=0;i<ReceiverNodes.size();i++)
        //            {
        //                if (MustDisable[ReceiverNodes[i]])continue;
        //                CanReceive[ReceiverNodes[i]]=true;
        //            }

        //            //add end node emitters
        //            std::vector<size_t> EndChoosenEmitters;
        //            GetChoosenEndNodeEmitters(EndChoosenEmitters);
        //            for (size_t i=0;i<EndChoosenEmitters.size();i++)
        //            {
        //                MustDisable[EndChoosenEmitters[i]]=false;
        //                CanEmit[EndChoosenEmitters[i]]=true;
        //            }

        //            //add end node receivers
        //            std::vector<size_t> EndChoosenReceivers;
        //            GetChoosenEndNodeReceivers(EndChoosenReceivers);
        //            for (size_t i=0;i<EndChoosenReceivers.size();i++)
        //            {
        //                MustDisable[EndChoosenReceivers[i]]=false;
        //                CanReceive[EndChoosenReceivers[i]]=true;
        //            }

        //            return;
        //        }

        if (TracingType==TraceLoop)
        {
            assert(FromType==Internal);
            assert(ToType==Internal);

            //get concave nodes
            std::vector<size_t> ConcaveNodes;
            GetConcaveNodes(ConcaveNodes);

            //narrow emitters
            std::vector<size_t> NarrowEmittersNodes;
            GetNarrowEmitters(NarrowEmittersNodes);

            //narrow receivers
            std::vector<size_t> NarrowReceiversNodes;
            GetNarrowReceivers(NarrowReceiversNodes);


            for (size_t i=0;i<ConcaveNodes.size();i++)
                MustDisable[ConcaveNodes[i]]=true;

            for (size_t i=0;i<NarrowEmittersNodes.size();i++)
                MustDisable[NarrowEmittersNodes[i]]=true;

            for (size_t i=0;i<NarrowReceiversNodes.size();i++)
                MustDisable[NarrowReceiversNodes[i]]=true;

            //set internal emitter
            std::vector<size_t> EmitterNodes;
            GetEmitterType(Internal,EmitterNodes);
            for (size_t i=0;i<EmitterNodes.size();i++)
            {
                if (MustDisable[EmitterNodes[i]]==false)
                    CanEmit[EmitterNodes[i]]=true;
            }

            return;
        }
        std::cout<<"****************************************"<<std::endl;
        PrintConfiguration(FromType,ToType,TracingType);
        std::cout<<"****************************************"<<std::endl;
        assert(0);
    }

    void UpdateChosenReceivers()
    {
        //std::cout<<"*** UPDATE  CHOSEN *** "<<std::endl;
        for (size_t i=0;i<ChoosenPaths.size();i++)
            for (size_t j=0;j<ChoosenPaths[i].PathNodes.size();j++)
            {
                size_t IndexN=ChoosenPaths[i].PathNodes[j];
                size_t IndexV=VertexFieldGraph<MeshType>::NodeVertI(IndexN);

                //change the type to choosen
                if ((VertType[IndexV]==Internal)||(VertType[IndexV]==None))
                    VertType[IndexV]=Choosen;

                //then add the two emitters
                size_t OrthoN0,OrthoN1;
                VertexFieldGraph<MeshType>::OrthoNode(IndexN,OrthoN0,OrthoN1);

                //safety check
                assert(NodeEmitterTypes[OrthoN0]!=Convex);

                bool CanChangedN0=((NodeEmitterTypes[OrthoN0]==Internal)||
                                   (NodeEmitterTypes[OrthoN0]==None))&&
                        ((NodeReceiverTypes[OrthoN0]==Internal)||
                         (NodeReceiverTypes[OrthoN0]==None));

                bool CanChangedN1=((NodeEmitterTypes[OrthoN1]==Internal)||
                                   (NodeEmitterTypes[OrthoN1]==None))&&
                        ((NodeReceiverTypes[OrthoN1]==Internal)||
                         (NodeReceiverTypes[OrthoN1]==None));

                if (VFGraph.IsActive(OrthoN0))
                {
                    if (CanChangedN0)
                    {
                        NodeEmitterTypes[OrthoN0]=Choosen;
                        NodeReceiverTypes[OrthoN0]=Choosen;
                    }
                }
                assert(NodeEmitterTypes[OrthoN1]!=Convex);

                if (VFGraph.IsActive(OrthoN1))
                {
                    if (CanChangedN1)
                    {
                        NodeEmitterTypes[OrthoN1]=Choosen;
                        NodeReceiverTypes[OrthoN1]=Choosen;
                    }
                }
            }
    }


    void InvalidateTracedNodes()
    {
        for (size_t i=0;i<ChoosenPaths.size();i++)
            for (size_t j=0;j<ChoosenPaths[i].PathNodes.size();j++)
            {
                size_t Node0=ChoosenPaths[i].PathNodes[j];
                size_t Node1=VertexFieldGraph<MeshType>::TangentNode(Node0);
                if (ChoosenPaths[i].IsLoop)
                {
                    VFGraph.SetActive(Node0,false);
                    VFGraph.SetActive(Node1,false);
                }
                else
                {
                    //first and last psecial threatment
                    if (j!=ChoosenPaths[i].PathNodes.size())
                        VFGraph.SetActive(Node0,false);
                    //first and last psecial threatment
                    if (j>0)
                        VFGraph.SetActive(Node1,false);
                }
            }
    }

    size_t UnsatisfiedNum()
    {
        size_t UnsatisfiedNum=0;
        for (size_t i=0;i<VerticesNeeds.size();i++)
            UnsatisfiedNum+=VerticesNeeds[i];
        return UnsatisfiedNum;
    }


    std::vector<ScalarType> CurrNodeDist;

    //    void GetFarthestPointFromSym(int &MinIdx,int &MaxIdx)
    //    {
    //        std::vector<vcg::Plane3<ScalarType> > Sym;
    //        std::cout<<"0"<<std::endl;
    //        vcg::tri::ExtrinsicPlaneSymmetry<MeshType> ExSym(Mesh());
    //        std::cout<<"1"<<std::endl;
    //        ExSym.Init();
    //        std::cout<<"2"<<std::endl;
    //        ExSym.GetPlanes(Sym,1);
    //        std::cout<<"3"<<std::endl;
    //        CoordType Dir0=Sym[0].Direction();
    //        Dir0.Normalize();
    //        CoordType Center=Mesh().bbox.Center();

    //        MinIdx=-1;
    //        MaxIdx=-1;
    //        ScalarType MinDot=1;
    //        ScalarType MaxDot=-1;
    //        for (size_t i=0;i<Mesh().vert.size();i++)
    //        {
    //            CoordType DirTest=Mesh().vert[i].P()-Center;

    //            ScalarType DotTest0=DirTest*Dir0;
    //            if (DotTest0>MaxDot)MaxIdx=i;

    //            ScalarType DotTest1=DirTest*(-Dir0);
    //            if (DotTest1<MinDot)MinIdx=i;
    //        }
    //        assert(MinIdx>=0);
    //        assert(MaxIdx>=0);
    //    }

    void GetFarthestPointFromSym(int &MinIdx,int &MaxIdx)
    {
        //        std::vector<size_t> InternalEmit;
        //        GetEmitterType(Internal,InternalEmit);
        //        ScalarType maxD=0;
        //        for (size_t i=0;i<InternalEmit.size()-1;i++)
        //            for (size_t j=(i+1);i<InternalEmit.size()-1;i++)
        //            {
        //                size_t NodeI0=InternalEmit[i];
        //                size_t NodeI1=InternalEmit[j];
        //                CoordType Pos0=VFGraph.NodePos(NodeI0);
        //                CoordType Pos1=VFGraph.NodePos(NodeI1);
        //                ScalarType testD=(Pos0-Pos1).Norm();
        //                if (testD<=maxD)continue;
        //                maxD=testD;
        //                MinIdx=VertexFieldGraph<MeshType>::NodeVertI(NodeI0);
        //                MaxIdx=VertexFieldGraph<MeshType>::NodeVertI(NodeI1);
        //            }
        //        std::vector<size_t> InternalEmit;
        //        GetEmitterType(Internal,InternalEmit);
        ScalarType maxD=0;
        for (size_t i=0;i<Mesh().vert.size()-1;i++)
            for (size_t j=(i+1);j<Mesh().vert.size();j++)
            {
                CoordType Pos0=VFGraph.Mesh().vert[i].P();
                CoordType Pos1=VFGraph.Mesh().vert[j].P();
                ScalarType testD=(Pos0-Pos1).Norm();
                if (testD<=maxD)continue;
                maxD=testD;
                MinIdx=i;
                MaxIdx=j;
            }
    }

    void InitDistances()
    {
        std::vector<size_t> Sources;

        for (size_t i=0;i<VFGraph.NumNodes();i++)
        {
            if (VFGraph.IsActive(i))continue;
            Sources.push_back(i);
        }
        for (size_t i=0;i<ChoosenPaths.size();i++)
        {
            std::vector<size_t> CurrPath=ChoosenPaths[i].PathNodes;
            Sources.insert(Sources.end(),CurrPath.begin(),CurrPath.end());
            VertexFieldGraph<MeshType>::TangentNodes(CurrPath);
            Sources.insert(Sources.end(),CurrPath.begin(),CurrPath.end());
        }

        std::vector<size_t> FlatEmit;
        GetFlatEmitters(FlatEmit);
        Sources.insert(Sources.end(),FlatEmit.begin(),FlatEmit.end());

        std::sort(Sources.begin(),Sources.end());
        std::vector<size_t>::iterator it;
        it = std::unique (Sources.begin(),Sources.end());
        Sources.resize( std::distance(Sources.begin(),it) );

        if (Sources.size()==0){
            int MinIdx,MaxIdx;
            std::cout<<"NO INITIAL FEATURE"<<std::endl;
            GetFarthestPointFromSym(MinIdx,MaxIdx);
            std::cout<<"Initialize with Points "<< MinIdx <<" and "<< MaxIdx <<std::endl;
            Sources.push_back(MinIdx);
            Sources.push_back(MaxIdx);
        }
        std::vector<bool> IsActive;
        VFGraph.IsActiveNodes(IsActive);
        VFGraph.SetAllActive();
        VertexFieldQuery<MeshType>::UpdateDistancesFrom(VFGraph,Sources,Drift);
        VFGraph.SetActiveNodes(IsActive);
        CurrNodeDist=VFGraph.Distances();
    }

    void RemoveNonActive(std::vector<size_t> &Nodes)
    {
        std::vector<size_t> SwapNodes;
        for (size_t i=0;i<Nodes.size();i++)
            if (VFGraph.IsActive(Nodes[i]))
                SwapNodes.push_back(Nodes[i]);
        Nodes=SwapNodes;
    }

    void UpdateDistancesWithLastChoosen()
    {
        std::vector<size_t> Sources;
        std::vector<size_t> CurrPath=ChoosenPaths.back().PathNodes;
        Sources=CurrPath;
        VertexFieldGraph<MeshType>::TangentNodes(CurrPath);
        Sources.insert(Sources.end(),CurrPath.begin(),CurrPath.end());
        RemoveNonActive(Sources);
        VertexFieldQuery<MeshType>::UpdateDistancesFrom(VFGraph,Sources,Drift,&CurrNodeDist);

        //InitDistances();
    }

    void SampleStartingNodes(bool OnSelectedFaces,
                             size_t sampleNum,
                             std::vector<size_t> &StartingNodes)
    {
        StartingNodes.clear();
        assert(!OnSelectedFaces);//TO BE IMPLEMENTED
        std::vector<CoordType> pointVec;
        ScalarType radius=0;
        std::cout<<"Poisson Sampling "<<sampleNum<<" Target samples"<<std::endl;
        vcg::tri::PoissonSampling<MeshType>(Mesh(),pointVec,sampleNum,radius,1,0.04f,276519752);
        std::vector<VertexType*> seedVec;
        vcg::tri::VoronoiProcessing<MeshType>::SeedToVertexConversion(Mesh(),pointVec,seedVec);
        for (size_t i=0;i<seedVec.size();i++)
        {
            size_t IndexV=vcg::tri::Index(Mesh(),seedVec[i]);
            std::vector<size_t> IndexN;
            VertexFieldGraph<MeshType>::IndexNodes(IndexV,IndexN);
            if(VFGraph.IsActive(IndexN[0]))
                StartingNodes.push_back(IndexN[0]);
            if(VFGraph.IsActive(IndexN[1]))
                StartingNodes.push_back(IndexN[1]);
        }
        std::cout<<"Sampled "<<StartingNodes.size()<<" samples"<<std::endl;
    }

    void InvalidateNodesType(TypeVert ToInvalid)
    {
        for (size_t i=0;i<VertType.size();i++)
        {
            if (VertType[i]==ToInvalid)
            {
                std::vector<size_t> Nodes;
                VertexFieldGraph<MeshType>::IndexNodes(i,Nodes);
                for (size_t j=0;j<Nodes.size();j++)
                    VFGraph.SetActive(Nodes[j],false);
            }
        }
    }


    void InvalidateEmittersType(TypeVert ToInvalid)
    {
        for (size_t i=0;i<NodeEmitterTypes.size();i++)
        {
            if (NodeEmitterTypes[i]==ToInvalid)
                VFGraph.SetActive(i,false);
        }
    }

    void InvalidateReceiversType(TypeVert ToInvalid)
    {
        for (size_t i=0;i<NodeReceiverTypes.size();i++)
        {
            if (NodeReceiverTypes[i]==ToInvalid)
                VFGraph.SetActive(i,false);
        }
    }

    void PrintConfiguration(TypeVert FromType,
                            TypeVert ToType,
                            TraceType TrType)
    {
        std::cout<<std::endl<<std::endl<<"** INFO **"<<std::endl;
        if (FromType==Narrow)
            std::cout<<"Tracing From Narrow ";
        if (FromType==Concave)
            std::cout<<"Tracing From Concave ";
        if (FromType==Flat)
            std::cout<<"Tracing From Flat ";
        if (FromType==Internal)
            std::cout<<"Tracing From Internal ";
        if (FromType==Choosen)
            std::cout<<"Tracing From Choosen ";

        if (ToType==Narrow)
            std::cout<<"To Narrow ";
        if (ToType==Concave)
            std::cout<<"To Concave ";
        if (ToType==Flat)
            std::cout<<"To Flat ";
        if (ToType==Internal)
            std::cout<<"To Internal ";
        if (ToType==Choosen)
            std::cout<<"To Choosen ";


        if (TrType==TraceDirect)
            std::cout<<"Method Direct "<<std::endl;
        if (TrType==DijkstraReceivers)
            std::cout<<"Method Dijkstra "<<std::endl;
        if (TrType==TraceLoop)
            std::cout<<"Method Loop "<<std::endl;
        std::cout<<"** END **"<<std::endl<<std::endl<<std::endl;
    }

    bool JoinConnection(TypeVert FromType,
                        TypeVert ToType,
                        TraceType TrType)
    {
        PrintConfiguration(FromType,ToType,TrType);

        //Candidates.clear();

        if ((FromType==Choosen)||(ToType==Choosen))
            UpdateChosenReceivers();

        std::vector<bool> CanEmit,CanReceive,MustDisable;
        GetConfiguration(FromType,ToType,TrType,CanEmit,CanReceive,MustDisable);

        VFGraph.SetAllActive();
        VFGraph.SetDisabledNodes(MustDisable);
        if ((FromType==Narrow)||(ToType==Narrow)
                ||(FromType==Concave)||(ToType==Concave))
            return(TraceFrom(FromType,ToType,TrType,CanEmit,CanReceive,Shortest));
        else
            return(TraceFrom(FromType,ToType,TrType,CanEmit,CanReceive,Fartest));
        //        std::cout<<"Adding candidates"<<std::endl;
        //        for (size_t i=0;i<VFGraph.NumNodes();i++)
        //        {
        //            //not the same kind
        //            if (!CanEmit[i])continue;

        //            //should be active
        //            assert(VFGraph.IsActive(i));

        //            Candidates.push_back(CandidateTrace(FromType,ToType,TrType,i));
        //        }
        //        std::cout<<"There are "<<Candidates.size()<<" Candidates "<<std::endl;
        //        std::cout<<"Updating candidates"<<std::endl;
        //        UpdateCandidates(CanReceive);

        //        std::cout<<"Before Expansion there are "<<Candidates.size()<<" candidates"<<std::endl;
        //        ExpandCandidates();

        //        if (Candidates.size()==0)return false;

        //        int size0=ChoosenPaths.size();
        //        std::cout<<"After Expansion there are "<<Candidates.size()<<" candidates"<<std::endl;
        //        if ((FromType==Narrow)||(FromType==Concave))
        //            ChooseGreedyByDistance(true);
        //        //ChooseGreedyByLengthVertNeeds(true);
        //        else
        //            ChooseGreedyByDistance(false);
        //        //ChooseGreedyByLengthVertNeeds(false);
        //        int size1=ChoosenPaths.size();
        //        std::cout<<"Choosen "<<size1-size0<<std::endl;

        //        return  ((size1-size0)>0);
    }


    void RetrievePartitioningFrom(const size_t &IndexF,std::vector<size_t> &partition)
    {
        partition.clear();

        std::vector<size_t> stack;
        std::vector<bool> explored(Mesh().face.size(),false);

        stack.push_back(IndexF);
        explored[IndexF]=true;
        do
        {
            size_t currF=stack.back();
            stack.pop_back();

            partition.push_back(currF);
            for (size_t j=0;j<3;j++)
            {
                if (Mesh().face[currF].IsFaceEdgeS(j))continue;

                int NextFIndex=vcg::tri::Index(Mesh(),Mesh().face[currF].FFp(j));

                if (explored[NextFIndex])continue;

                explored[NextFIndex]=true;
                stack.push_back(NextFIndex);
            }
        }while (!stack.empty());
    }

    bool SelectBorders()
    {
        //std::set<std::pair<size_t,size_t> > BorderPatches;
        vcg::tri::UpdateFlags<MeshType>::FaceClearFaceEdgeS(Mesh());
        //first add borders
        for (size_t i=0;i<Mesh().face.size();i++)
            for (size_t j=0;j<3;j++)
            {
                if (!Mesh().face[i].IsB(j))continue;
                Mesh().face[i].SetFaceEdgeS(j);
            }

        std::set<std::pair<size_t,size_t> > BorderEdges;
        for (size_t i=0;i<ChoosenPaths.size();i++)
        {
            if (ChoosenPaths[i].PathNodes.size()==0)continue;

            size_t Limit=ChoosenPaths[i].PathNodes.size()-1;
            if (ChoosenPaths[i].IsLoop)Limit++;
            for (size_t j=0;j<Limit;j++)
            {
                size_t IndexN0=ChoosenPaths[i].PathNodes[j];
                size_t IndexN1=ChoosenPaths[i].PathNodes[(j+1)%ChoosenPaths[i].PathNodes.size()];
                size_t IndexV0=VertexFieldGraph<MeshType>::NodeVertI(IndexN0);
                size_t IndexV1=VertexFieldGraph<MeshType>::NodeVertI(IndexN1);
                BorderEdges.insert(std::pair<size_t,size_t>(std::min(IndexV0,IndexV1),
                                                            std::max(IndexV0,IndexV1)));
            }
        }

        std::vector<size_t> NumSel(Mesh().vert.size(),0);
        for (size_t i=0;i<Mesh().face.size();i++)
            for (size_t j=0;j<3;j++)
            {
                size_t IndexV0=vcg::tri::Index(Mesh(),Mesh().face[i].V0(j));
                size_t IndexV1=vcg::tri::Index(Mesh(),Mesh().face[i].V1(j));
                std::pair<size_t,size_t> Key(std::min(IndexV0,IndexV1),std::max(IndexV0,IndexV1));
                if (BorderEdges.count(Key)==0)continue;
                Mesh().face[i].SetFaceEdgeS(j);
                NumSel[IndexV0]++;
                NumSel[IndexV1]++;
            }

        for (size_t i=0;i<NumSel.size();i++)
        {
            if (Mesh().vert[i].IsB())continue;
            assert(NumSel[i]%2==0);
            if (NumSel[i]==2)return false;
        }
        return true;
    }

public:
    std::vector<std::vector<size_t> > Partitions;
    std::vector<int > FacePartitions;

    std::vector<PatchType> PartitionType;
    //std::vector<ScalarType> PartitionARatio;
    std::vector<std::vector<size_t> > PartitionCorners;

private:
    struct EdgeVert
    {
        size_t EV0;
        size_t EV1;
        size_t CurrV;

        EdgeVert(size_t _EV0,size_t _EV1,size_t _CurrV)
        {
            EV0=std::min(_EV0,_EV1);
            EV1=std::max(_EV0,_EV1);
            CurrV=_CurrV;
        }

        inline bool operator ==(const EdgeVert &left)const
        {
            return ((EV0==left.EV0)&&
                    (EV1==left.EV1)&&
                    (CurrV==left.CurrV));
        }

        inline bool operator <(const EdgeVert &left)const
        {
            if ((EV0==left.EV0)&&
                    (EV1==left.EV1))
                return (CurrV<left.CurrV);
            if (EV0==left.EV0)
                return (EV1<left.EV1);
            return (EV0<left.EV0);
        }
    };

    void FindPartitionsCorners()
    {
        PartitionCorners.clear();
        PartitionCorners.resize(Partitions.size());

        //for each edge set the direction per vertex
        std::map<EdgeVert,size_t> EdgeDirVert;
        for (size_t i=0;i<ChoosenPaths.size();i++)
        {
            if (ChoosenPaths[i].PathNodes.size()==0)continue;
            size_t Limit=ChoosenPaths[i].PathNodes.size()-1;
            if (ChoosenPaths[i].IsLoop)
                Limit++;
            for (size_t j=0;j<Limit;j++)
            {
                size_t IndexN0=ChoosenPaths[i].PathNodes[j];
                size_t IndexN1=ChoosenPaths[i].PathNodes[(j+1)%ChoosenPaths[i].PathNodes.size()];
                size_t IndexV0=VertexFieldGraph<MeshType>::NodeVertI(IndexN0);
                size_t IndexV1=VertexFieldGraph<MeshType>::NodeVertI(IndexN1);
                size_t DirV0=VertexFieldGraph<MeshType>::NodeDirI(IndexN0);
                size_t DirV1=VertexFieldGraph<MeshType>::NodeDirI(IndexN1);
                EdgeVert EdgeKey0(IndexV0,IndexV1,IndexV0);
                EdgeVert EdgeKey1(IndexV0,IndexV1,IndexV1);

                if (EdgeDirVert.count(EdgeKey0)>0)
                {
                    //std::cout<<"WARING DOUBLE DIRECTION"<<std::endl;
                }
                else
                    EdgeDirVert[EdgeKey0]=(DirV0%2);

                if (EdgeDirVert.count(EdgeKey1)>0)
                {
                    //std::cout<<"WARING DOUBLE DIRECTION"<<std::endl;
                }
                else
                    EdgeDirVert[EdgeKey1]=(DirV1%2);
            }
        }

        //do the same for borders
        for (size_t i=0;i<Mesh().face.size();i++)
            for (size_t j=0;j<3;j++)
            {
                if (!vcg::face::IsBorder(Mesh().face[i],j))continue;
                size_t IndexV0=vcg::tri::Index(Mesh(),Mesh().face[i].V0(j));
                size_t IndexV1=vcg::tri::Index(Mesh(),Mesh().face[i].V1(j));
                size_t DirFlatV0,DirFlatV1;
                GetEdgeDir(IndexV0,IndexV1,DirFlatV0,DirFlatV1);

                EdgeVert EdgeKey0(IndexV0,IndexV1,IndexV0);
                EdgeVert EdgeKey1(IndexV0,IndexV1,IndexV1);

                assert(EdgeDirVert.count(EdgeKey0)==0);
                EdgeDirVert[EdgeKey0]=DirFlatV0 % 2;
                assert(EdgeDirVert.count(EdgeKey1)==0);
                EdgeDirVert[EdgeKey1]= DirFlatV1 % 2;
            }

        //then go over all partitions
        for (size_t i=0;i<Partitions.size();i++)
        {
            //map for each vertex of the partitions the number of directions
            //that have been sampled
            std::map<size_t,std::set<size_t> > DirNode;
            for (size_t j=0;j<Partitions[i].size();j++)
            {
                size_t IndexF=Partitions[i][j];
                for (size_t e=0;e<3;e++)
                {
                    size_t IndexV0=vcg::tri::Index(Mesh(),Mesh().face[IndexF].V0(e));
                    size_t IndexV1=vcg::tri::Index(Mesh(),Mesh().face[IndexF].V1(e));

                    if (VertType[IndexV0]==Convex)
                        PartitionCorners[i].push_back(IndexV0);
                    else
                    {
                        EdgeVert EdgeKey0(IndexV0,IndexV1,IndexV0);
                        if (EdgeDirVert.count(EdgeKey0)>0)
                        {
                            size_t EdgeDir=EdgeDirVert[EdgeKey0];
                            //std::cout<<"EdgeDir "<<EdgeDir<<std::endl;
                            DirNode[IndexV0].insert(EdgeDir);
                        }
                    }

                    if (VertType[IndexV1]==Convex)
                        PartitionCorners[i].push_back(IndexV1);
                    else
                    {
                        EdgeVert EdgeKey1(IndexV0,IndexV1,IndexV1);
                        if (EdgeDirVert.count(EdgeKey1)>0)
                        {
                            size_t EdgeDir=EdgeDirVert[EdgeKey1];
                            DirNode[IndexV1].insert(EdgeDir);
                        }
                    }
                }
            }

            //then add the corner
            for (size_t j=0;j<Partitions[i].size();j++)
            {
                size_t IndexF=Partitions[i][j];
                for (size_t e=0;e<3;e++)
                {
                    size_t IndexV=vcg::tri::Index(Mesh(),Mesh().face[IndexF].V(e));
                    size_t NumDirV=DirNode[IndexV].size();
                    if (NumDirV>1)
                        PartitionCorners[i].push_back(IndexV);
                }
            }

            std::sort(PartitionCorners[i].begin(),PartitionCorners[i].end());
            std::vector<size_t>::iterator it;
            it = std::unique (PartitionCorners[i].begin(),PartitionCorners[i].end());
            PartitionCorners[i].resize( std::distance(PartitionCorners[i].begin(),it) );
        }
    }

    void UpdatePartitionType(size_t Index)
    {
        assert(Index<PartitionCorners.size());
        assert(Index<Partitions.size());

        //PartitionARatio[Index]=-1;

        //check for incomplete
        for (size_t i=0;i<Partitions[Index].size();i++)
        {
            size_t IndexF=Partitions[Index][i];
            for (size_t j=0;j<3;j++)
            {
                size_t IndexV=vcg::tri::Index(Mesh(),Mesh().face[IndexF].V(j));
                if (VerticesNeeds[IndexV]>0)
                {
                    PartitionType[Index]=HasEmitter;
                    return;
                }
            }
        }

        //check for number of corners
        if (PartitionCorners[Index].size()<MinVal)
        {
            PartitionType[Index]=LowCorners;
            return;
        }
        if (PartitionCorners[Index].size()>MaxVal)
        {
            PartitionType[Index]=HighCorners;
            return;
        }

        //copy the mesh
        vcg::tri::UpdateFlags<MeshType>::FaceClearS(Mesh());
        for (size_t i=0;i<Partitions[Index].size();i++)
        {
            size_t IndexF=Partitions[Index][i];
            Mesh().face[IndexF].SetS();
        }

        int Genus=Mesh().GenusOfSelectedFaces();
        if (Genus!=1)
        {
            PartitionType[Index]=NonDisk;
            return;
        }

        PartitionType[Index]=IsOK;
        //PartitionARatio
    }

    void InitPartitionsType()
    {
        PartitionType.clear();
        PartitionType.resize(Partitions.size(),IsOK);
        for (size_t i=0;i<Partitions.size();i++)
            UpdatePartitionType(i);
    }


    void RetrievePartitionsFaces(std::vector<size_t> &StartF)
    {
        Partitions.clear();
        vcg::tri::UpdateFlags<MeshType>::FaceClearV(Mesh());
        for (size_t i=0;i<StartF.size();i++)
        {
            size_t IndexF=StartF[i];
            if (Mesh().face[IndexF].IsV())continue;

            std::vector<size_t> partition;
            RetrievePartitioningFrom(IndexF,partition);

            for (size_t j=0;j<partition.size();j++)
                Mesh().face[partition[j]].SetV();

            Partitions.push_back(partition);
        }
    }

public:
    void RetrievePartitionsFromChoosen(bool UpdateType=true)
    {
        //size_t t0=clock();
        bool IsOk=SelectBorders();
        assert(IsOk);
        std::vector<size_t> StartF;
        for (size_t i=0;i<Mesh().face.size();i++)
            StartF.push_back(i);

        Partitions.clear();

        RetrievePartitionsFaces(StartF);

        FacePartitions.clear();
        FacePartitions.resize(Mesh().face.size(),-1);

        for (size_t i=0;i<Partitions.size();i++)
            for (size_t j=0;j<Partitions[i].size();j++)
            {
                size_t IndexF=Partitions[i][j];
                FacePartitions[IndexF]=i;
            }
        //size_t t1=clock();
        FindPartitionsCorners();
        //      size_t t2=clock();
        //      std::cout<<"ta.0 "<<t1-t0<<std::endl;
        //      std::cout<<"ta.1 "<<t2-t1<<std::endl;
        if (UpdateType)
            InitPartitionsType();
    }

public:

    bool split_on_removal;
    bool avoid_increase_valence;
    bool avoid_collapse_irregular;
    ScalarType max_lenght_distortion;
    ScalarType max_lenght_variance;
    ScalarType sample_ratio;
    size_t MinVal;
    size_t MaxVal;

    void ColorByPartitions()
    {
        for (size_t i=0;i<Partitions.size();i++)
        {
            vcg::Color4b CurrCol=vcg::Color4b::Scatter(Partitions.size(),i);
            for (size_t j=0;j<Partitions[i].size();j++)
                Mesh().face[Partitions[i][j]].C()=CurrCol;
        }
    }

    void ColorByPatchQuality()
    {
        RetrievePartitionsFromChoosen(false);
        std::vector<ScalarType> PartitionQ(Partitions.size(),0);
        ScalarType MaxQ=0;
        ScalarType LenghtVar=0;
        for (size_t i=0;i<Partitions.size();i++)
        {
            PatchQuality(Partitions[i],PartitionCorners[i],PartitionQ[i],LenghtVar);
            MaxQ=std::max(MaxQ,PartitionQ[i]);
        }
        for (size_t i=0;i<Partitions.size();i++)
        {
            vcg::Color4b Col=vcg::Color4b::ColorRamp(1,MaxQ,PartitionQ[i]);
            for (size_t j=0;j<Partitions[i].size();j++)
            {
                size_t IndexF=Partitions[i][j];
                Mesh().face[IndexF].C()=Col;
            }
        }
        std::cout<<"Worst Quality "<<MaxQ<<std::endl;
    }

    void ColorByValence()
    {
        //vcg::tri::UpdateSelection<MeshType>::FaceClear(Mesh());
        for (size_t i=0;i<Partitions.size();i++)
        {
            vcg::Color4b CurrCol=vcg::Color4b::Gray;

            if (PartitionCorners[i].size()==MinVal)
                CurrCol=vcg::Color4b::Yellow;
            if (PartitionCorners[i].size()==5)
                CurrCol=vcg::Color4b::Blue;
            if (PartitionCorners[i].size()==MaxVal)
                CurrCol=vcg::Color4b::Cyan;
            if ((PartitionCorners[i].size()<MinVal)||
                    (PartitionCorners[i].size()>MaxVal))
                CurrCol=vcg::Color4b::Red;


            for (size_t j=0;j<Partitions[i].size();j++)
                Mesh().face[Partitions[i][j]].C()=CurrCol;
        }
    }

    void ColorByTopology()
    {
        for (size_t i=0;i<Partitions.size();i++)
        {
            vcg::Color4b CurrCol=vcg::Color4b::Gray;

            if (PartitionType[i]==LowCorners)
                CurrCol=vcg::Color4b::Blue;

            if (PartitionType[i]==HighCorners)
                CurrCol=vcg::Color4b::Blue;

            if (PartitionType[i]==NonDisk)
                CurrCol=vcg::Color4b::Red;

            if (PartitionType[i]==HasEmitter)
                CurrCol=vcg::Color4b::Yellow;

            for (size_t j=0;j<Partitions[i].size();j++)
                Mesh().face[Partitions[i][j]].C()=CurrCol;
        }
    }

private:

    bool HasIncompleteEmitter()
    {
        for (size_t i=0;i<PartitionType.size();i++)
            if (PartitionType[i]==HasEmitter)return true;
        return false;
    }

    bool HasTerminated()
    {
        for (size_t i=0;i<PartitionType.size();i++)
            if (PartitionType[i]!=IsOK)return false;
        return true;
    }

    bool HasConcaveNarrowVert(size_t IndexPath)
    {
        for (size_t j=0;j<ChoosenPaths[IndexPath].PathNodes.size();j++)
        {
            size_t IndexN=ChoosenPaths[IndexPath].PathNodes[j];
            size_t IndexV=VertexFieldGraph<MeshType>::NodeVertI(IndexN);
            if ((VertType[IndexV]==Narrow)||(VertType[IndexV]==Concave))
                return true;
        }
        return false;
    }

    //    bool HasTJunction(size_t IndexPath)
    //    {
    //        std::vector<size_t> DirV[2];
    //        DirV[0].resize(Mesh().vert.size(),0);
    //        DirV[1].resize(Mesh().vert.size(),0);
    //        for (size_t i=0;i<ChoosenPaths.size();i++)
    //        {
    //            if (ChoosenPaths[i].PathNodes.size()==0)continue;
    //            if (ChoosenPaths[i].IsLoop)continue;
    //            size_t NodeI0=ChoosenPaths[i].PathNodes[0];
    //            size_t NodeI1=ChoosenPaths[i].PathNodes.back();
    //            size_t Dir0=VertexFieldGraph<MeshType>::NodeDirI(NodeI0)%2;
    //            size_t Dir1=VertexFieldGraph<MeshType>::NodeDirI(NodeI1)%2;
    //            size_t VertI0=VertexFieldGraph<MeshType>::NodeVertI(NodeI0);
    //            size_t VertI1=VertexFieldGraph<MeshType>::NodeVertI(NodeI1);
    //            DirV[Dir0][VertI0]+=1;
    //            DirV[Dir1][VertI1]+=1;
    //        }

    //        for (size_t i=0;i<ChoosenPaths[IndexPath].PathNodes.size();i++)
    //        {
    //            size_t NodeI=ChoosenPaths[IndexPath].PathNodes[i];
    //            size_t VertI=VertexFieldGraph<MeshType>::NodeVertI(NodeI);
    //            size_t Dir=VertexFieldGraph<MeshType>::NodeDirI(NodeI)%2;
    //            size_t CrossDir=(Dir+1)%2;
    //            //if cross with a sharp feature then not check
    //            if (Mesh().vert[VertI].IsB())continue;
    //            if (DirV[CrossDir][VertI]==1)return true;
    //            if ((DirV[Dir][VertI]==2)&&(DirV[CrossDir][VertI]==0))return true;
    //        }
    //        return false;
    //    }

    bool HasTJunction()
    {
        std::vector<size_t> PathV;
        PathV.resize(Mesh().vert.size(),0);
        for (size_t i=0;i<ChoosenPaths.size();i++)
        {
            if (ChoosenPaths[i].PathNodes.size()==0)continue;
            size_t limit=ChoosenPaths[i].PathNodes.size()-1;
            if (ChoosenPaths[i].IsLoop)limit++;
            size_t node_num=ChoosenPaths[i].PathNodes.size();
            for (size_t j=0;j<limit;j++)
            {
                size_t NodeI0=ChoosenPaths[i].PathNodes[j];
                size_t NodeI1=ChoosenPaths[i].PathNodes[(j+1)%node_num];
                size_t VertI0=VertexFieldGraph<MeshType>::NodeVertI(NodeI0);
                size_t VertI1=VertexFieldGraph<MeshType>::NodeVertI(NodeI1);
                PathV[VertI0]++;
                PathV[VertI1]++;
            }
        }
        for (size_t i=0;i<PathV.size();i++)
        {
            if (Mesh().vert[i].IsB())continue;
            if (PathV[i]==1)return true;
        }
        return false;
    }

    void ReverseTrace(CandidateTrace &Trace)
    {
        std::reverse(Trace.PathNodes.begin(),Trace.PathNodes.end());
        for (size_t i=0;i<Trace.PathNodes.size();i++)
            Trace.PathNodes[i]=VertexFieldGraph<MeshType>::TangentNode(Trace.PathNodes[i]);
        std::swap(Trace.FromType,Trace.ToType);
    }

    bool MergeContiguousStep()
    {
        for (size_t i=0;i<ChoosenPaths.size();i++)
            for (size_t j=0;j<ChoosenPaths.size();j++)
            {
                if (i==j)continue;
                if (ChoosenPaths[i].IsLoop)continue;
                if (ChoosenPaths[j].IsLoop)continue;
                if(ChoosenPaths[i].PathNodes.size()==0)continue;
                if(ChoosenPaths[j].PathNodes.size()==0)continue;
                size_t Node0_0=ChoosenPaths[i].PathNodes[0];
                size_t Node0_1=ChoosenPaths[i].PathNodes.back();
                size_t Node1_0=ChoosenPaths[j].PathNodes[0];
                size_t Node1_1=ChoosenPaths[j].PathNodes.back();
                if (Node0_1==Node1_0)
                {
                    //remove last one
                    ChoosenPaths[i].PathNodes.pop_back();
                    //add the rest
                    ChoosenPaths[i].PathNodes.insert(ChoosenPaths[i].PathNodes.end(),
                                                     ChoosenPaths[j].PathNodes.begin(),
                                                     ChoosenPaths[j].PathNodes.end());
                    ChoosenPaths[i].ToType=ChoosenPaths[j].ToType;

                    ChoosenPaths[j].PathNodes.clear();
                    return true;
                }
                if (Node1_1==Node0_0)
                {
                    //remove last one
                    ChoosenPaths[j].PathNodes.pop_back();
                    //add the rest
                    ChoosenPaths[j].PathNodes.insert(ChoosenPaths[j].PathNodes.end(),
                                                     ChoosenPaths[i].PathNodes.begin(),
                                                     ChoosenPaths[i].PathNodes.end());

                    ChoosenPaths[j].ToType=ChoosenPaths[i].ToType;

                    ChoosenPaths[i].PathNodes.clear();
                    return true;
                }
            }
        return false;
    }

    void MergeContiguousPaths()
    {
        while(MergeContiguousStep()){};
    }


    void PatchBorders(const std::vector<size_t> &FacesIdx,
                      const std::vector<size_t> &CornerIdx,
                      std::vector<std::vector<vcg::face::Pos<FaceType> > > &Borders)
    {
        //std::cout<<"A"<<std::endl;
        //select corners
        vcg::tri::UpdateSelection<MeshType>::VertexClear(Mesh());
        for (size_t i=0;i<CornerIdx.size();i++)
            Mesh().vert[CornerIdx[i]].SetS();

        //select face
        //std::cout<<"B"<<std::endl;
        vcg::tri::UpdateSelection<MeshType>::FaceClear(Mesh());
        for (size_t i=0;i<FacesIdx.size();i++)
            Mesh().face[FacesIdx[i]].SetS();

        //std::cout<<"C"<<std::endl;
        for (size_t i=0;i<Mesh().face.size();i++)
            for (size_t j=0;j<3;j++)
                Mesh().face[i].SetF(j);

        //std::cout<<"D"<<std::endl;
        for (size_t i=0;i<FacesIdx.size();i++)
        {
            int IndexF=FacesIdx[i];
            FaceType *f0=&Mesh().face[IndexF];
            for (size_t j=0;j<3;j++)
            {
                if (f0->IsB(j))
                    f0->ClearF(j);
                else
                {
                    FaceType *FOpp=f0->FFp(j);
                    if (FOpp->IsS())continue;
                    f0->ClearF(j);
                }
            }
        }

        //std::cout<<"E"<<std::endl;
        vcg::face::Pos<FaceType> StartPos;
        bool found=false;
        for (size_t i=0;i<FacesIdx.size();i++)
        {
            int IndexF=FacesIdx[i];
            FaceType *f0=&Mesh().face[IndexF];
            for (size_t j=0;j<3;j++)
            {
                if (f0->IsF(j))continue;
                VertexType *v0=f0->V0(j);
                VertexType *v1=f0->V1(j);
                StartPos.F()=f0;
                StartPos.E()=j;
                if (v0->IsS())
                {
                    StartPos.V()=v0;
                    found=true;
                    break;
                }
                if (v1->IsS())
                {
                    StartPos.V()=v1;
                    found=true;
                    break;
                }
            }
            if (found)break;
        }
        assert(found);


        //std::cout<<"F"<<std::endl;
        Borders.clear();
        Borders.resize(1);
        StartPos.FlipV();
        vcg::face::Pos<FaceType> CurrPos=StartPos;
        //then iterate
        do{
            Borders.back().push_back(CurrPos);

            if (CurrPos.V()->IsS())
            {
                Borders.resize(Borders.size()+1);
            }
            CurrPos.NextNotFaux();
        }while (CurrPos!=StartPos);
        //last one inserted empty
        Borders.pop_back();
    }


    void SequenceLenght(const std::vector<vcg::face::Pos<FaceType> > &PosSeq,
                        ScalarType &TangentL, ScalarType &EuclideanL)
    {
        TangentL=0;
        for (size_t i=0;i<PosSeq.size();i++)
        {
            CoordType Edge=(PosSeq[i].V()->P()-PosSeq[i].VFlip()->P());
            TangentL+=Edge.Norm();
        }
        EuclideanL=(PosSeq.back().V()->P()-PosSeq[0].VFlip()->P()).Norm();
    }

    void SplitSubSequences(const std::vector<vcg::face::Pos<FaceType> > &PosSeq,
                           std::vector<std::vector<vcg::face::Pos<FaceType> > > &SplitSeq)
    {
        SplitSeq.clear();
        SplitSeq.resize(1);
        for (size_t i=0;i<PosSeq.size()-1;i++)
        {
            SplitSeq.back().push_back(PosSeq[i]);
            if ((PosSeq[i].IsBorder()!=PosSeq[i+1].IsBorder()))
                SplitSeq.resize(SplitSeq.size()+1);
        }
        SplitSeq.back().push_back(PosSeq.back());
    }

    void PosLenght(const std::vector<vcg::face::Pos<FaceType> > &PosSeq,
                   std::vector<ScalarType> &TangentL,
                   std::vector<ScalarType> &EuclideanL)
    {
        std::vector<std::vector<vcg::face::Pos<FaceType> > > SplitSeq;
        SplitSubSequences(PosSeq,SplitSeq);

        TangentL.resize(SplitSeq.size());
        EuclideanL.resize(SplitSeq.size());
        for (size_t i=0;i<SplitSeq.size();i++)
            SequenceLenght(SplitSeq[i],TangentL[i],EuclideanL[i]);

    }


    void PatchQuality(std::vector<size_t> &FacesIdx,
                      std::vector<size_t> &CornerIdx,
                      ScalarType &LenghtRatio,
                      ScalarType &LenghtVariance)
    {
        std::vector<std::vector<vcg::face::Pos<FaceType> > > Borders;
        PatchBorders(FacesIdx,CornerIdx,Borders);
        LenghtRatio=1;
        std::vector<ScalarType> SideL;
        for (size_t i=0;i<Borders.size();i++)
        {
            assert(Borders[i].size()>=1);
            std::vector<ScalarType> TangentL,EuclideanL;
            PosLenght(Borders[i],TangentL,EuclideanL);

            ScalarType CurrSideL=0;
            for (size_t j=0;j<TangentL.size();j++)
                CurrSideL+=TangentL[j];

            SideL.push_back(CurrSideL);

            for (size_t j=0;j<TangentL.size();j++)
            {
                ScalarType CurrRatio=(TangentL[j]/EuclideanL[j]);
                //                if (CurrRatio<1)
                //                {
                //                    std::cout<<"WARNING "<<CurrRatio<<std::endl;
                //                    assert(CurrRatio>=1);
                //                }
                LenghtRatio=std::max(LenghtRatio,CurrRatio);
            }
        }

        ScalarType MaxLenght=SideL[0];
        ScalarType MinLenght=SideL[0];
        for (size_t i=1;i<SideL.size();i++)
        {
            MaxLenght=std::max(MaxLenght,SideL[i]);
            MinLenght=std::min(MinLenght,SideL[i]);
        }

        LenghtVariance=MaxLenght/MinLenght;
        //        AVGLenght/=Borders.size();
        //        LenghtVariance=1;
        //        for (size_t i=0;i<SideL.size();i++)
        //        {
        //            ScalarType Ratio=std::max(SideL[i],AVGLenght)/ std::min(SideL[i],AVGLenght);
        //            LenghtVariance=std::max(Ratio,LenghtVariance);
        //        }
    }

    void WorstPatchQuality(ScalarType &WorstLenght,
                           ScalarType &WorstLVariance,
                           bool only_irregular=true)
    {
        WorstLenght=1;
        WorstLVariance=1;
        for (size_t i=0;i<Partitions.size();i++)
        {
            if (only_irregular && (PartitionCorners[i].size()==4))continue;
            ScalarType CurrLenght,CurrVariance;
            PatchQuality(Partitions[i],PartitionCorners[i],CurrLenght,CurrVariance);
            WorstLenght=std::max(WorstLenght,CurrLenght);
            WorstLVariance=std::max(WorstLVariance,CurrVariance);
        }
    }

    void GetCurrentConfigurationAround(const std::vector<vcg::face::Pos<FaceType> > &FacesPath,
                                       bool twoSides,
                                       size_t &NonProper,
                                       size_t &Valence3,
                                       size_t &Valence5,
                                       size_t &Valence6)
    {
        NonProper=0;
        Valence3=0;
        Valence5=0;
        Valence6=0;
        //get the indexes of faces
        std::vector<size_t> IdxFaces;
        for (size_t i=0;i<FacesPath.size();i++)
        {
            IdxFaces.push_back(vcg::tri::Index(Mesh(),FacesPath[i].F()));
            if (twoSides)
                IdxFaces.push_back(vcg::tri::Index(Mesh(),FacesPath[i].FFlip()));
        }
        //std::cout<<"2-Selected Path Pos "<<IdxFaces.size()<<std::endl;
        //then retrieve partitions
        RetrievePartitionsFaces(IdxFaces);
        //std::cout<<"There are "<<PartitionType.size()<<" partitions"<<std::endl;
        //find corners
        FindPartitionsCorners();
        //find type
        InitPartitionsType();
        //std::cout<<"There are "<<PartitionType.size()<<" partitions"<<std::endl;
        for (size_t i=0;i<PartitionType.size();i++)
        {
            switch(PartitionType[i]) {
            case LowCorners:
                NonProper++;
                break;
            case HighCorners:
                NonProper++;
                break;
            case NonDisk:
                NonProper++;
                break;
            case HasEmitter:
                NonProper++;
                break;
            default : break;
            }
        }
        for (size_t i=0;i<PartitionCorners.size();i++)
        {
            if (PartitionCorners[i].size()==3)Valence3++;
            if (PartitionCorners[i].size()==5)Valence5++;
            if (PartitionCorners[i].size()==6)Valence6++;
        }
    }


    bool RemoveIfPossible(size_t IndexPath)
    {
        if (ChoosenPaths[IndexPath].PathNodes.size()==0)return false;
        //if it includes a concave or narrow then cannot remove
        if (HasConcaveNarrowVert(IndexPath))return false;
        //check if have t junction in the middle
        //if (HasTJunction(IndexPath))return false;
        //CHECK ENDPOINTS!
        assert(IndexPath<ChoosenPaths.size());

        //get the old configuration
        std::vector<vcg::face::Pos<FaceType> > FacesPath;
        //std::cout<<"TEST BEFORE"<<std::endl;
        VFGraph.GetNodesPos(ChoosenPaths[IndexPath].PathNodes,ChoosenPaths[IndexPath].IsLoop,FacesPath);
        //std::cout<<"Selected Path Pos "<<FacesPath.size()<<std::endl;
        size_t NonProper0,Valence3_0,Valence5_0,Valence6_0;
        GetCurrentConfigurationAround(FacesPath,true,NonProper0,Valence3_0,Valence5_0,Valence6_0);
        //max_lenght_distortion=1.2;
        //exit(0);
        //test removal
        CandidateTrace OldTr=ChoosenPaths[IndexPath];
        ChoosenPaths[IndexPath].PathNodes.clear();
        //check Tjunctions
        if (HasTJunction())
        {
            //restore
            ChoosenPaths[IndexPath]=OldTr;
            Mesh().SelectPos(FacesPath,true);
            return false;
        }
        //deselect
        Mesh().SelectPos(FacesPath,false);
        size_t NonProper1,Valence3_1,Valence5_1,Valence6_1;
        //std::cout<<"TEST AFTER"<<std::endl;
        GetCurrentConfigurationAround(FacesPath,true,NonProper1,Valence3_1,Valence5_1,Valence6_1);

        for (size_t i=0;i<PartitionCorners.size();i++)
            if (PartitionCorners[i].size()<=2)
            {
                //restore
                ChoosenPaths[IndexPath]=OldTr;
                Mesh().SelectPos(FacesPath,true);
                return false;
            }

        bool CanRemove=true;
        if (NonProper1>NonProper0)CanRemove=false;
        if (avoid_increase_valence)
        {
            if (Valence6_1>Valence6_0)CanRemove=false;
            if (Valence5_1>Valence5_0)CanRemove=false;
            if (Valence3_1>Valence3_0)CanRemove=false;
        }
        if (avoid_collapse_irregular)
        {
            if (Valence6_0>0)CanRemove=false;
            if (Valence5_0>0)CanRemove=false;
            if (Valence3_0>0)CanRemove=false;
        }
        if (CanRemove)
        {
            ScalarType LenghtDist;
            ScalarType LenghtVariance;
            WorstPatchQuality(LenghtDist,LenghtVariance);
            if ((max_lenght_distortion>1) &&(LenghtDist>max_lenght_distortion))CanRemove=false;
            if ((max_lenght_variance>1) && (LenghtVariance>max_lenght_variance))CanRemove=false;
        }
        if (!CanRemove)
        {
            //restore
            ChoosenPaths[IndexPath]=OldTr;
            Mesh().SelectPos(FacesPath,true);
            return false;
        }
        return true;
    }

    void RemoveEmptyPaths()
    {
        std::vector<CandidateTrace> ChoosenPathsSwap;
        //remove the ones with no nodes
        for (size_t i=0;i<ChoosenPaths.size();i++)
        {
            if (ChoosenPaths[i].PathNodes.size()==0)continue;
            ChoosenPathsSwap.push_back(ChoosenPaths[i]);
        }
        ChoosenPaths=ChoosenPathsSwap;
    }


    bool RemoveIteration()
    {
        bool HasRemoved=false;

        for (int i=ChoosenPaths.size()-1;i>=0;i--)
        {
            std::cout<<"Removing "<<i<<" if Possible"<<std::endl;
            HasRemoved|=RemoveIfPossible(i);
        }

        MergeContiguousPaths();
        std::cout<<"DONE!"<<std::endl;
        //        if (HasRemoved)
        //        {
        //        MergeContiguousPaths();
        RemoveEmptyPaths();
        return HasRemoved;
    }

    void SplitIntoSubPathsBySel(const CandidateTrace &ToSplit,
                                std::vector<CandidateTrace> &Portions)
    {
        Portions.clear();
        int StartI=0;
        if (ToSplit.IsLoop)
        {
            StartI=-1;
            for (size_t j=0;j<ToSplit.PathNodes.size();j++)
            {
                size_t IndexV=VertexFieldGraph<MeshType>::NodeVertI(ToSplit.PathNodes[j]);
                if (Mesh().vert[IndexV].IsS())
                {
                    StartI=j;
                    break;
                }
            }

            //no crossing at all
            if (StartI==-1)
            {
                Portions.push_back(ToSplit);
                return;
            }
        }
        assert(StartI>=0);
        assert(StartI<ToSplit.PathNodes.size());
        size_t numNodes=ToSplit.PathNodes.size();
        std::vector<std::vector<size_t> > SubPaths;
        SubPaths.resize(1);
        int CurrI=StartI;
        do
        {
            size_t CurrN=ToSplit.PathNodes[CurrI];
            size_t IndexV=VertexFieldGraph<MeshType>::NodeVertI(CurrN);
            size_t NextI=(CurrI+1)%numNodes;

            //then add a new subpath
            if ((Mesh().vert[IndexV].IsS())&&(CurrI!=StartI))
            {
                SubPaths.back().push_back(CurrN);
                SubPaths.resize(SubPaths.size()+1);
            }
            SubPaths.back().push_back(CurrN);
            CurrI=NextI;
        }while (CurrI!=StartI);

        //add the first one in case is loop
        if (ToSplit.IsLoop)
            SubPaths.back().push_back(ToSplit.PathNodes[StartI]);

        //in case not loop and last one was selected then remove the last
        if ((!ToSplit.IsLoop)&&(SubPaths.back().size()==1))
            SubPaths.pop_back();

        //        std::cout<<"Max Size "<<SubPaths.size()<<std::endl;
        //        if (ToSplit.IsLoop)
        //            std::cout<<"Loop "<<std::endl;
        for (size_t i=0;i<SubPaths.size();i++)
        {
            //            std::cout<<"Indeex "<<i<<std::endl;
            //            std::cout<<"Size "<<SubPaths[i].size()<<std::endl;
            assert(SubPaths[i].size()>=2);
            size_t Node0=SubPaths[i][0];
            size_t Node1=SubPaths[i].back();
            size_t IndexV0=VertexFieldGraph<MeshType>::NodeVertI(Node0);
            size_t IndexV1=VertexFieldGraph<MeshType>::NodeVertI(Node1);
            TypeVert T0=VertType[IndexV0];
            TypeVert T1=VertType[IndexV1];
            Portions.push_back(CandidateTrace(T0,T1,ToSplit.TracingMethod,Node0));
            Portions.back().PathNodes=SubPaths[i];
            Portions.back().IsLoop=false;
            Portions.back().Updated=true;
        }
        //case of a loop with a single intersection, then keep the original
        if ((Portions.size()==1)&&(ToSplit.IsLoop))
        {
            Portions.clear();
            Portions.push_back(ToSplit);
        }
    }

    void SelectCrossIntersections()
    {
        vcg::tri::UpdateSelection<MeshType>::VertexClear(Mesh());
        vcg::tri::UpdateQuality<MeshType>::VertexConstant(Mesh(),0);
        //count occourence
        for (size_t i=0;i<ChoosenPaths.size();i++)
            for (size_t j=0;j<ChoosenPaths[i].PathNodes.size();j++)
            {
                size_t IndexV=VertexFieldGraph<MeshType>::NodeVertI(ChoosenPaths[i].PathNodes[j]);
                Mesh().vert[IndexV].Q()+=1;
            }
        for (size_t i=0;i<Mesh().vert.size();i++)
        {
            if (Mesh().vert[i].Q()>=2)Mesh().vert[i].SetS();
            if (Mesh().vert[i].IsB() && (Mesh().vert[i].Q()>0))Mesh().vert[i].SetS();
        }
    }


public:

    void SplitIntoSubPaths()
    {
        SelectCrossIntersections();
        std::vector<CandidateTrace> NewTraces;
        for (size_t i=0;i<ChoosenPaths.size();i++)
        {
            std::vector<CandidateTrace> Portions;
            SplitIntoSubPathsBySel(ChoosenPaths[i],Portions);
            NewTraces.insert(NewTraces.end(),Portions.begin(),Portions.end());
        }
        ChoosenPaths.clear();
        ChoosenPaths=NewTraces;

        for (size_t i=0;i<ChoosenPaths.size();i++)
        {
            //std::cout<<ChoosenPaths[i].PathNodes.size()<<std::endl;
            assert(ChoosenPaths[i].PathNodes.size()>=2);
        }

        RetrievePartitionsFromChoosen();
        vcg::tri::UpdateSelection<MeshType>::Clear(Mesh());
        ColorByPartitions();
    }

public:

    struct PatchInfoType
    {
        size_t LowC;
        size_t HighC;
        size_t NonDiskLike;
        size_t HasEmit;
        size_t NumPatchs;
        size_t SizePatches[8];
    };

    void GetInfo(PatchInfoType &PInfo)
    {
        PInfo.LowC=0;
        PInfo.HighC=0;
        PInfo.NonDiskLike=0;
        PInfo.HasEmit=0;
        PInfo.NumPatchs=PartitionType.size();
        PInfo.SizePatches[0]=0;
        PInfo.SizePatches[1]=0;
        PInfo.SizePatches[2]=0;
        PInfo.SizePatches[3]=0;
        PInfo.SizePatches[4]=0;
        PInfo.SizePatches[5]=0;
        PInfo.SizePatches[6]=0;
        PInfo.SizePatches[7]=0;

        for (size_t i=0;i<PartitionType.size();i++)
        {
            switch(PartitionType[i]) {
            case LowCorners  :
                PInfo.LowC++;
                break;
            case HighCorners  :
                PInfo.HighC++;
                break;
            case NonDisk  :
                PInfo.NonDiskLike++;
                break;
            case HasEmitter  :
                PInfo.HasEmit++;
                break;
                //            case MoreSing  :
                //                HasMoreSing++;
                //                break;
                //            case IsOK  :
                //                break;
            }
        }
        for (size_t i=0;i<PartitionCorners.size();i++)
        {
            size_t numC=PartitionCorners[i].size();
            numC=std::min(numC,(size_t)7);
            PInfo.SizePatches[numC]++;
        }
    }

    void WriteInfo()
    {

        PatchInfoType PInfo;
        GetInfo(PInfo);
        std::cout<<"***FINAL STATS***"<<std::endl;
        std::cout<<"* Num Patches "<<PInfo.NumPatchs<<std::endl;
        std::cout<<"* Low Valence Patches "<<PInfo.LowC<<std::endl;
        std::cout<<"* High Valence Patches "<<PInfo.HighC<<std::endl;
        std::cout<<"* Non Disk Like Patches "<<PInfo.NonDiskLike<<std::endl;
        std::cout<<"* With Emitters Patches "<<PInfo.HasEmit<<std::endl;
        for (size_t i=0;i<8;i++)
            std::cout<<"* Patch with  "<<i<<" corners are: "<<PInfo.SizePatches[i]<<std::endl;
        //std::cout<<"* With More Singularities "<<HasMoreSing<<std::endl;
    }

    void RemovePaths(bool DoSmooth=true)
    {
        std::vector<std::vector<vcg::face::Pos<FaceType> > > PathPos;

        //select pos
        vcg::tri::UpdateFlags<MeshType>::FaceClearFaceEdgeS(Mesh());
        GetPathPos(PathPos);
        Mesh().SelectPos(PathPos,true);

        if (DoSmooth)
            SmoothPatches(10);

        std::cout<<"Removing..."<<std::endl;
        while (RemoveIteration()){}

        RetrievePartitionsFromChoosen(true);
        ColorByPartitions();
        WriteInfo();

        //MergeContiguousPaths();
        //ColorByPatchQuality();
    }

    void GetCornersPos(std::vector<CoordType> &PatchCornerPos)
    {
        PatchCornerPos.clear();
        for (size_t i=0;i<Partitions.size();i++)
        {
            std::set<size_t> CurrPartV(PartitionCorners[i].begin(),
                                       PartitionCorners[i].end());
            for (size_t j=0;j<Partitions[i].size();j++)
            {
                size_t IndexF=Partitions[i][j];
                CoordType bary=(Mesh().face[IndexF].P(0)+
                                Mesh().face[IndexF].P(1)+
                                Mesh().face[IndexF].P(2))/3;
                for (size_t e=0;e<3;e++)
                {
                    size_t IndexV=vcg::tri::Index(Mesh(),Mesh().face[IndexF].V(e));
                    CoordType VertPos=Mesh().vert[IndexV].P();
                    if (CurrPartV.count(IndexV)==0)continue;
                    PatchCornerPos.push_back(bary*0.5+VertPos*0.5);
                }
            }
        }
    }

    void GetNarrowActiveEmitters(std::vector<size_t> &AllEmit)
    {
        AllEmit.clear();
        UpdateChosenReceivers();
        for (size_t i=0;i<NodeEmitterTypes.size();i++)
        {
            if (!VFGraph.IsActive(i))continue;
            if (NodeEmitterTypes[i]==Narrow)
                AllEmit.push_back(i);
        }
    }

    void GetNarrowActiveReceivers(std::vector<size_t> &AllReceive)
    {
        AllReceive.clear();
        UpdateChosenReceivers();
        for (size_t i=0;i<NodeReceiverTypes.size();i++)
        {
            if (!VFGraph.IsActive(i))continue;
            if (NodeReceiverTypes[i]==Narrow)
                AllReceive.push_back(i);
        }
    }

    void GetChoosenEmitters(std::vector<size_t> &AllEmit)
    {
        AllEmit.clear();
        UpdateChosenReceivers();
        for (size_t i=0;i<NodeEmitterTypes.size();i++)
        {
            if (!VFGraph.IsActive(i))continue;
            if (NodeEmitterTypes[i]==Choosen)
                AllEmit.push_back(i);
        }
    }

    void GetChoosenReceivers(std::vector<size_t> &AllReceive)
    {
        AllReceive.clear();
        UpdateChosenReceivers();
        for (size_t i=0;i<NodeReceiverTypes.size();i++)
        {
            if (!VFGraph.IsActive(i))continue;
            if (NodeReceiverTypes[i]==Choosen)
                AllReceive.push_back(i);
        }
    }


    void GetConcaveEmitters(std::vector<size_t> &AllEmit)
    {
        AllEmit.clear();
        UpdateChosenReceivers();
        std::vector<TypeVert > NodeEmitterTypes0=NodeEmitterTypes;
        std::vector<TypeVert> NodeReceiverTypes0=NodeReceiverTypes;
        UpdateChosenReceivers();
        assert(NodeEmitterTypes0==NodeEmitterTypes);
        assert(NodeReceiverTypes0==NodeReceiverTypes);

        for (size_t i=0;i<NodeEmitterTypes.size();i++)
        {
            if (!VFGraph.IsActive(i))continue;
            if (NodeEmitterTypes[i]==Concave)
                AllEmit.push_back(i);
        }
    }

    void GetConcaveReceivers(std::vector<size_t> &AllReceive)
    {
        AllReceive.clear();
        UpdateChosenReceivers();
        for (size_t i=0;i<NodeReceiverTypes.size();i++)
        {
            if (!VFGraph.IsActive(i))continue;
            if (NodeReceiverTypes[i]==Concave)
                AllReceive.push_back(i);
        }
    }

    void GetFlatEmitters(std::vector<size_t> &AllEmit)
    {
        AllEmit.clear();
        UpdateChosenReceivers();
        for (size_t i=0;i<NodeEmitterTypes.size();i++)
        {
            if (!VFGraph.IsActive(i))continue;
            if (NodeEmitterTypes[i]==Flat)
                AllEmit.push_back(i);
        }
    }

    void GetFlatReceivers(std::vector<size_t> &AllReceive)
    {
        AllReceive.clear();
        UpdateChosenReceivers();
        for (size_t i=0;i<NodeReceiverTypes.size();i++)
        {
            if (!VFGraph.IsActive(i))continue;
            if (NodeReceiverTypes[i]==Flat)
                AllReceive.push_back(i);
        }
    }

    void Init(ScalarType _Drift)
    {
        Drift=_Drift;
        InitStructures();
        ChoosenPaths.clear();
        //ChoosenIsLoop.clear();
        MaxNarrowWeight=sqrt(TotArea(Mesh()))*MAX_NARROW_CONST*Drift;

        RetrievePartitionsFromChoosen();
        ColorByPartitions();

        //        std::vector<size_t> NarrowEmit;
        //        GetNarrowActiveEmitters(NarrowEmit);
        //        std::cout<<"Emitters Narrow "<<NarrowEmit.size()<<std::endl;
    }

    void CopyFrom(PatchTracer<MeshType> &Ptr,
                  std::vector<size_t> &VertMap,
                  size_t IndexPatch)
    {
        Drift=Ptr.Drift;
        MaxNarrowWeight=Ptr.MaxNarrowWeight;

        //get the vertices type
        assert(VertMap.size()==Mesh().vert.size());

        //set the vert type
        VertType.resize(Mesh().vert.size(),None);
        VerticesNeeds.resize(Mesh().vert.size(),0);

        //copy the original vert type
        for (size_t i=0;i<VertMap.size();i++)
        {
            size_t IndexV=VertMap[i];
            assert(IndexV<Ptr.Mesh().vert.size());
            assert(IndexV<Ptr.VertType.size());
            VertType[i]=Ptr.VertType[IndexV];
            VerticesNeeds[i]=Ptr.VerticesNeeds[IndexV];
        }


        //update coherently for the new patch
        for (size_t i=0;i<VertType.size();i++)
        {
            if ((VertType[i]!=Narrow)&&
                    (VertType[i]!=Concave)&&
                    Mesh().vert[i].IsB())
                VertType[i]=Flat;

            //            if (VertType[i]==Internal)
            //                VertType[i]=None;

            if ((VertType[i]==Narrow)&&(VerticesNeeds[i]==0))
                VertType[i]=Flat;

            if ((VertType[i]==Concave)&&(VerticesNeeds[i]==0))
                VertType[i]=Flat;
        }

        //then also set the corners as convex
        std::set<size_t> CornerSet(Ptr.PartitionCorners[IndexPatch].begin(),
                                   Ptr.PartitionCorners[IndexPatch].end());
        for (size_t i=0;i<VertType.size();i++)
        {
            size_t IndexV=VertMap[i];
            if (CornerSet.count(IndexV)>0)
                VertType[i]=Convex;
        }

        //get configuration on borders
        std::vector<size_t> FlatEmitters,FlatReceivers,ChosenEmitters,
                ChosenReceivers,FlatTangent,ChosenTangent;

        Ptr.GetPatchBorderNodes(IndexPatch,FlatEmitters,FlatReceivers,ChosenEmitters,
                                ChosenReceivers,FlatTangent,ChosenTangent);

        //initialize the emitters
        NodeEmitterTypes.clear();
        NodeReceiverTypes.clear();
        NodeEmitterTypes.resize(VFGraph.NumNodes(),None);
        NodeReceiverTypes.resize(VFGraph.NumNodes(),None);

        //        //initialize internals for loops
        //        size_t sampleNum=floor(sqrt(Mesh().vert.size())+0.5)*10*sample_ratio;
        //        sampleNum=std::max(sampleNum,(size_t)50);
        //        std::vector<size_t> StartingNodes;
        //        SampleStartingNodes(false,sampleNum,StartingNodes);
        //        for (size_t i=0;i<StartingNodes.size();i++)
        //        {
        //            size_t IndexV=VertexFieldGraph<MeshType>::NodeVertI(StartingNodes[i]);
        //            //std::cout<<"Sampled V "<<IndexV<<std::endl;
        //            if (VertType[IndexV]!=Internal)continue;
        //            assert(NodeEmitterTypes[StartingNodes[i]]==None);
        //            NodeEmitterTypes[StartingNodes[i]]=Internal;
        //        }

        FlatEmitters.insert(FlatEmitters.end(),ChosenEmitters.begin(),ChosenEmitters.end());
        FlatReceivers.insert(FlatReceivers.end(),ChosenReceivers.begin(),ChosenReceivers.end());

        std::set<std::pair<size_t,size_t> > FlattenEmitSet;
        std::set<std::pair<size_t,size_t> > FlattenReceiveSet;

        for (size_t i=0;i<FlatEmitters.size();i++)
        {
            size_t IndexV,IndexDir;
            VertexFieldGraph<MeshType>::VertDir(FlatEmitters[i],IndexV,IndexDir);
            FlattenEmitSet.insert(std::pair<size_t,size_t>(IndexV,IndexDir));
        }
        for (size_t i=0;i<FlatReceivers.size();i++)
        {
            size_t IndexV,IndexDir;
            VertexFieldGraph<MeshType>::VertDir(FlatReceivers[i],IndexV,IndexDir);
            FlattenReceiveSet.insert(std::pair<size_t,size_t>(IndexV,IndexDir));
        }

        //        std::set<size_t> EmitterSet(FlatEmitters.begin(),FlatEmitters.end());
        //        std::set<size_t> ReceiverSet(FlatReceivers.begin(),FlatReceivers.end());


        //then adjust concave or convex
        for (size_t i=0;i<VertType.size();i++)
        {
            if (VertType[i]==Flat)
            {
                size_t OrigV=VertMap[i];
                for (size_t Dir=0;Dir<4;Dir++)
                {
                    std::pair<size_t,size_t> Key(OrigV,Dir);
                    size_t IndexN=VertexFieldGraph<MeshType>::IndexNode(i,Dir);
                    if (FlattenReceiveSet.count(Key)>0)
                    {
                        NodeReceiverTypes[IndexN]=Flat;
                        //assert(FlattenEmitSet.count(Key)==0);
                    }else
                        if (FlattenEmitSet.count(Key)>0)
                        {
                            NodeEmitterTypes[IndexN]=Flat;
                            //assert(FlattenReceiveSet.count(Key)==0);
                        }
                }
            }
            else
                //in this case copy from original
                if ((VertType[i]==Narrow)||(VertType[i]==Concave))
                {
                    std::vector<size_t> Nodes0;
                    VertexFieldGraph<MeshType>::IndexNodes(i,Nodes0);

                    size_t IndexV=VertMap[i];
                    std::vector<size_t> Nodes1;
                    VertexFieldGraph<MeshType>::IndexNodes(IndexV,Nodes1);

                    assert(Nodes0.size()==4);
                    assert(Nodes1.size()==4);
                    for (size_t i=0;i<Nodes0.size();i++)
                    {
                        NodeEmitterTypes[Nodes0[i]]=Ptr.NodeEmitterTypes[Nodes1[i]];
                        NodeReceiverTypes[Nodes0[i]]=Ptr.NodeReceiverTypes[Nodes1[i]];
                    }
                }
        }
    }

    void GetPathPos(std::vector<std::vector<vcg::face::Pos<FaceType> > > &Paths)
    {
        Paths.clear();
        Paths.resize(ChoosenPaths.size());
        for (size_t i=0;i<ChoosenPaths.size();i++)
        {
            VFGraph.GetNodesPos(ChoosenPaths[i].PathNodes,
                                ChoosenPaths[i].IsLoop,
                                Paths[i]);
        }

    }

    //    void SmoothPatches(size_t Steps=3,ScalarType Damp=0.5)
    //    {
    //        MeshType TargetMesh;
    //        vcg::tri::Append<MeshType,MeshType>::Mesh(TargetMesh,Mesh());
    //        TargetMesh.UpdateAttributes();
    //        vcg::GridStaticPtr<FaceType,ScalarType> Gr;
    //        Gr.Set(TargetMesh.face.begin(),TargetMesh.face.end());

    //        for (size_t i=0;i<Mesh().vert.size();i++)
    //            if (Mesh().vert[i].IsB())Mesh().vert[i].SetS();


    //        //get paths
    //        std::vector<std::vector<vcg::face::Pos<FaceType> > > Paths;
    //        GetPathPos(Paths);
    //        std::vector<bool> IsLoop;
    //        GetCurrChosenIsLoop(IsLoop);

    //        //smooth the path
    //        for (size_t s=0;s<Steps;s++)
    //        {
    //            std::vector<CoordType> AvPos(Mesh().vert.size(),CoordType(0,0,0));
    //            std::vector<size_t> NumDiv(Mesh().vert.size(),0);

    //            //smooth path
    //            for (size_t i=0;i<Paths.size();i++)
    //            {
    //                for (size_t j=0;j<Paths[i].size();j++)
    //                {
    //                    FaceType *f=Paths[i][j].F();
    //                    size_t IndexE=Paths[i][j].E();
    //                    VertexType *V0=f->V0(IndexE);
    //                    VertexType *V1=f->V1(IndexE);
    //                    size_t IndexV0=vcg::tri::Index(Mesh(),V0);
    //                    size_t IndexV1=vcg::tri::Index(Mesh(),V1);
    //                    CoordType Pos0=Mesh().vert[IndexV0].P();
    //                    CoordType Pos1=Mesh().vert[IndexV1].P();
    //                    CoordType Dir=Pos0-Pos1;
    //                    Dir.Normalize();
    //                    AvPos[IndexV0]+=Pos1;
    //                    NumDiv[IndexV0]++;
    //                    AvPos[IndexV1]+=Pos0;
    //                    NumDiv[IndexV1]++;
    //                }
    //            }
    //            for (size_t i=0;i<Mesh().vert.size();i++)
    //            {
    //                //fixed
    //                if (Mesh().vert[i].IsS())continue;
    //                //no contributes
    //                if (NumDiv[i]==0)continue;

    //                CoordType TargetPos=(AvPos[i]/(ScalarType)NumDiv[i]);
    //                Mesh().vert[i].P()=(Mesh().vert[i].P()*Damp) +
    //                        ((TargetPos)*(1-Damp));

    //                Mesh().vert[i].SetV();
    //            }
    //            //smooth the rest
    //            for (size_t i=0;i<Mesh().face.size();i++)
    //                for (size_t j=0;j<3;j++)
    //                {
    //                    size_t IndexV0=vcg::tri::Index(Mesh(),Mesh().face[i].V0(j));
    //                    size_t IndexV1=vcg::tri::Index(Mesh(),Mesh().face[i].V1(j));
    //                    CoordType Pos0=Mesh().vert[IndexV0].P();
    //                    CoordType Pos1=Mesh().vert[IndexV1].P();
    //                    if (!Mesh().vert[IndexV0].IsV())
    //                    {
    //                        AvPos[IndexV0]+=Pos1;
    //                        NumDiv[IndexV0]++;
    //                    }
    //                    if (!Mesh().vert[IndexV1].IsV())
    //                    {
    //                        AvPos[IndexV1]+=Pos0;
    //                        NumDiv[IndexV1]++;
    //                    }
    //                }
    //            for (size_t i=0;i<Mesh().vert.size();i++)
    //            {
    //                //fixed
    //                if (Mesh().vert[i].IsS())continue;
    //                if (Mesh().vert[i].IsV())continue;
    //                //no contributes
    //                if (NumDiv[i]==0)continue;
    //                Mesh().vert[i].P()=(Mesh().vert[i].P()*Damp) +
    //                        ((AvPos[i]/NumDiv[i])*(1-Damp));
    //            }

    //            //reproject everything
    //            for (size_t i=0;i<Mesh().vert.size();i++)
    //            {
    //                //fixed
    //                CoordType Pos;
    //                ScalarType MinD;
    //                vcg::tri::GetClosestFaceBase(TargetMesh,Gr,Mesh().vert[i].P(),Mesh().bbox.Diag(),MinD,Pos);
    //                if (Mesh().vert[i].IsS())continue;
    //                Mesh().vert[i].P()=Pos;
    //            }
    //        }
    //        vcg::tri::UpdateSelection<MeshType>::VertexClear(Mesh());
    //        Mesh().UpdateAttributes();
    //    }

    void GetPatchNodes(const size_t &IndexPatch,
                       std::vector<size_t> &PatchNodes)
    {
        for (size_t i=0;i<Partitions[IndexPatch].size();i++)
        {
            size_t FaceI=Partitions[IndexPatch][i];
            for (size_t j=0;j<3;j++)
            {
                size_t VertI=vcg::tri::Index(Mesh(),Mesh().face[FaceI].V(j));
                std::vector<size_t> NodeI;
                VertexFieldGraph<MeshType>::IndexNodes(VertI,NodeI);
                PatchNodes.insert(PatchNodes.end(),NodeI.begin(),NodeI.end());
            }
        }
        std::sort(PatchNodes.begin(),PatchNodes.end());
        auto last=std::unique(PatchNodes.begin(),PatchNodes.end());
        PatchNodes.erase(last, PatchNodes.end());
    }

    enum ChooseMode{Fartest,Shortest};

    bool TraceFrom(TypeVert FromType,
                   TypeVert ToType,
                   TraceType TrType,
                   const std::vector<bool> &CanEmit,
                   const std::vector<bool> &CanReceive,
                   const ChooseMode ChMode,
                   bool checkOnlylastConfl=false)
    {
        Candidates.clear();

        std::cout<<"Adding candidates"<<std::endl;
        for (size_t i=0;i<VFGraph.NumNodes();i++)
        {
            //not the same kind
            if (!CanEmit[i])continue;

            //should be active
            assert(VFGraph.IsActive(i));

            Candidates.push_back(CandidateTrace(FromType,ToType,TrType,i));
        }
        std::cout<<"There are "<<Candidates.size()<<" Candidates "<<std::endl;
        std::cout<<"Updating candidates"<<std::endl;
        UpdateCandidates(CanReceive);

        std::cout<<"Before Expansion there are "<<Candidates.size()<<" candidates"<<std::endl;
        ExpandCandidates();

        if (Candidates.size()==0)return false;

        int size0=ChoosenPaths.size();

        std::cout<<"After Expansion there are "<<Candidates.size()<<" candidates"<<std::endl;
        bool UseNodeVal=((FromType==Narrow)||(FromType==Concave));
        if (ChMode==Fartest)
            ChooseGreedyByDistance(UseNodeVal);
        else
            ChooseGreedyByLengthVertNeeds(UseNodeVal);
        //ChooseGreedyByLengthVertNeeds(false,checkOnlylastConfl);

        int size1=ChoosenPaths.size();
        std::cout<<"Choosen "<<size1-size0<<std::endl;

        return  ((size1-size0)>0);
    }

    std::vector<size_t> FilterFromSet(const std::vector<size_t> &ToFilter,
                                      const std::set<size_t> &FilterSet)
    {
        std::vector<size_t> Filtered;
        for (size_t i=0;i<ToFilter.size();i++)
        {
            if(FilterSet.count(ToFilter[i])==0)continue;
            Filtered.push_back(ToFilter[i]);
        }
        return Filtered;
    }

    void GetPatchBorderNodes(const size_t &IndexPatch,
                             std::vector<size_t> &FlatEmitters,
                             std::vector<size_t> &FlatReceivers,
                             std::vector<size_t> &ChosenEmitters,
                             std::vector<size_t> &ChosenReceivers,
                             std::vector<size_t> &FlatTangent,
                             std::vector<size_t> &ChosenTangent)
    {
        FlatEmitters.clear();
        FlatReceivers.clear();
        ChosenEmitters.clear();
        ChosenReceivers.clear();
        FlatTangent.clear();
        ChosenTangent.clear();

        std::vector<size_t> PatchNodes;
        GetPatchNodes(IndexPatch,PatchNodes);
        std::set<size_t> PatchNodesSet(PatchNodes.begin(),PatchNodes.end());


        GetFlatEmitters(FlatEmitters);
        GetFlatReceivers(FlatReceivers);
        //just filters the one in the patch
        FlatEmitters=FilterFromSet(FlatEmitters,PatchNodesSet);
        FlatReceivers=FilterFromSet(FlatReceivers,PatchNodesSet);

        GetFlatTangentNodes(FlatTangent);
        FlatTangent=FilterFromSet(FlatTangent,PatchNodesSet);

        //then get the one from choosen
        std::vector<size_t> ChoosenNodes;
        GetChoosenNodes(ChoosenNodes);
        ChoosenNodes=FilterFromSet(ChoosenNodes,PatchNodesSet);

        GetChoosenNodesAndTangent(ChosenTangent);
        ChosenTangent=FilterFromSet(ChosenTangent,PatchNodesSet);

        //then for each one get the ortho nodes
        for (size_t i=0;i<ChoosenNodes.size();i++)
        {
            size_t OrthoN0,OrthoN1;
            VertexFieldGraph<MeshType>::OrthoNode(ChoosenNodes[i],OrthoN0,OrthoN1);
            assert(OrthoN0!=OrthoN1);
            std::vector<size_t> NodeNeigh0,NodeNeigh1;

            VFGraph.GetNodeNeigh(OrthoN0,NodeNeigh0);
            NodeNeigh0=FilterFromSet(NodeNeigh0,PatchNodesSet);

            VFGraph.GetNodeNeigh(OrthoN1,NodeNeigh1);
            NodeNeigh1=FilterFromSet(NodeNeigh1,PatchNodesSet);

            if (NodeNeigh0.size()>NodeNeigh1.size())
            {
                ChosenEmitters.push_back(OrthoN0);
                ChosenReceivers.push_back(OrthoN1);
            }
            else
            {
                ChosenEmitters.push_back(OrthoN1);
                ChosenReceivers.push_back(OrthoN0);
            }
        }
    }

    //    bool TraceWithinPatch(const size_t &IndexPatch,
    //                          const size_t Step,
    //                          bool AllReceivers)
    //    {
    //        assert(Step>=0);
    //        assert(Step<3);

    //        std::vector<size_t> PatchNodes;
    //        GetPatchNodes(IndexPatch,PatchNodes);
    //        std::set<size_t> PatchNodesSet(PatchNodes.begin(),PatchNodes.end());

    //        std::vector<size_t> FlatEmitters,FlatReceivers,ChosenEmitters,
    //                            ChosenReceivers,FlatTangent,ChosenTangent;

    //        GetPatchBorderNodes(IndexPatch,FlatEmitters,FlatReceivers,ChosenEmitters,
    //                            ChosenReceivers,FlatTangent,ChosenTangent);

    ////        std::cout<<"Choosen Emitters Size "<<ChosenEmitters.size()<<std::endl;
    ////        std::cout<<"Choosen Receivers Size "<<ChosenReceivers.size()<<std::endl;
    ////        std::cout<<"Flat Emitters Size "<<FlatEmitters.size()<<std::endl;
    ////        std::cout<<"Flat Receivers Size "<<FlatReceivers.size()<<std::endl;
    ////        //get all nodes and different category
    ////        std::vector<size_t> ChoosenNodes;
    ////        GetChoosenNodes(ChoosenNodes);

    ////        //then get the tangent
    ////        std::vector<size_t> TangentChoosenNodes=ChoosenNodes;
    ////        VertexFieldGraph<MeshType>::TangentNodes(TangentChoosenNodes);

    //////        std::vector<size_t> TangentFlatNodes;
    //////        GetFlatTangentNodes(TangentFlatNodes);
    //////        std::vector<size_t> ChoosenOrthoNodes;
    //////        VertexFieldGraph<MeshType>::OrthoNodes(ChoosenTangentNodes,ChoosenOrthoNodes);

    ////        std::vector<size_t> ChoosenReceivers;
    ////        std::vector<size_t> ChoosenEmitters;

    ////        std::vector<size_t> FlatReceivers;
    ////        GetFlatReceivers(FlatReceivers);

    ////        std::vector<size_t> FlatEmitters;
    ////        GetFlatEmitters(FlatEmitters);

    //        //1 disable all but the one in the patch
    //        std::vector<bool> MustDisable(VFGraph.NumNodes(),true);
    //        for (size_t i=0;i<PatchNodes.size();i++)
    //            MustDisable[PatchNodes[i]]=false;

    //        //2: disable all corners (only if not in AllReceiverMode)
    //        for (size_t i=0;i<PartitionCorners[IndexPatch].size();i++)
    //        {
    //            std::vector<size_t> NodeI;
    //            VertexFieldGraph<MeshType>::IndexNodes(PartitionCorners[IndexPatch][i],NodeI);
    //            for (size_t j=0;j<NodeI.size();j++)
    //                MustDisable[NodeI[j]]=true;
    //        }

    //        //3: disable all the tangent nodes to paths (only if not in AllReceiverMode)
    //        if (!AllReceivers)
    //        {
    //            for (size_t i=0;i<FlatTangent.size();i++)
    //                MustDisable[FlatTangent[i]]=true;

    //            for (size_t i=0;i<ChosenTangent.size();i++)
    //                MustDisable[ChosenTangent[i]]=true;
    //        }

    //        //4 set all receivers

    ////        std::vector<size_t> PossibleReceivers(ChoosenOrthoNodes.begin(),ChoosenOrthoNodes.end());
    ////        PossibleReceivers.insert(PossibleReceivers.end(),FlatOrthoNodes.begin(),FlatOrthoNodes.end());
    ////        if (AllReceivers)
    ////        {
    ////            PossibleReceivers.insert(PossibleReceivers.end(),ChoosenTangentNodes.begin(),ChoosenTangentNodes.end());
    ////            PossibleReceivers.insert(PossibleReceivers.end(),TangentFlatNodes.begin(),TangentFlatNodes.end());
    ////        }

    ////        //set all receivers
    ////        std::vector<size_t> Receivers;
    ////        for (size_t i=0;i<PossibleReceivers.size();i++)
    ////        {
    ////            size_t IndexN=PossibleReceivers[i];
    ////            if(PatchNodesSet.count(IndexN)==0)continue;
    ////            if (MustDisable[IndexN])continue;
    ////            Receivers.push_back(IndexN);
    ////        }
    ////        std::cout<<"There are "<<Receivers.size()<<" receivers"<<std::endl;

    //        std::vector<bool> CanReceive(VFGraph.NumNodes(),false);
    //        for (size_t i=0;i<FlatReceivers.size();i++)
    //            CanReceive[FlatReceivers[i]]=true;
    //        for (size_t i=0;i<ChosenReceivers.size();i++)
    //            CanReceive[ChosenReceivers[i]]=true;
    //        if (AllReceivers)
    //        {
    //            for (size_t i=0;i<FlatTangent.size();i++)
    //                CanReceive[FlatTangent[i]]=true;
    //            for (size_t i=0;i<ChosenTangent.size();i++)
    //                CanReceive[ChosenTangent[i]]=true;
    //        }
    //        //5: then get the emitters
    //        std::vector<bool> CanEmit(VFGraph.NumNodes(),false);
    //        std::vector<size_t> EmitterNodes;

    //        if (Step==0)
    //        {
    //            GetEmitterType(Narrow,EmitterNodes);
    //            EmitterNodes=FilterFromSet(EmitterNodes,PatchNodesSet);
    //            for (size_t i=0;i<EmitterNodes.size();i++)
    //                MustDisable[EmitterNodes[i]]=false;
    //        }
    //        if (Step==1)
    //        {
    //            GetEmitterType(Concave,EmitterNodes);
    //            EmitterNodes=FilterFromSet(EmitterNodes,PatchNodesSet);
    //            for (size_t i=0;i<EmitterNodes.size();i++)
    //                MustDisable[EmitterNodes[i]]=false;
    //        }
    //        if (Step==2)
    //        {
    //            EmitterNodes=FlatEmitters;
    //            EmitterNodes.insert(EmitterNodes.end(),ChosenEmitters.begin(),ChosenEmitters.end());
    ////            for (size_t i=0;i<ChoosenOrthoNodes.size();i++)
    ////            {
    ////                size_t IndexV=ChoosenOrthoNodes[i];
    ////                if (MustDisable[IndexV])continue;
    ////                PossibleEmitterNodes.push_back(IndexV);
    ////            }
    ////            for (size_t i=0;i<FlatOrthoNodes.size();i++)
    ////            {
    ////                size_t IndexV=FlatOrthoNodes[i];
    ////                if (MustDisable[IndexV])continue;
    ////                PossibleEmitterNodes.push_back(IndexV);
    ////            }
    //        }

    //        for (size_t i=0;i<EmitterNodes.size();i++)
    //        {
    //            //if (PatchNodesSet.count(EmitterNodes[i])==0)continue;
    //            CanEmit[EmitterNodes[i]]=true;
    //            //only for flat ones
    //            MustDisable[EmitterNodes[i]]=false;
    //        }

    //        //6: do the trace
    //        VFGraph.SetAllActive();
    //        VFGraph.SetDisabledNodes(MustDisable);
    //        bool HasTraced=false;

    //        MaxNarrowWeight*=1000;

    //        TypeVert FromType=Choosen;

    //        if (Step==0)
    //            FromType=Narrow;
    //        if (Step==1)
    //            FromType=Concave;

    //        HasTraced=TraceFrom(FromType,Choosen,DijkstraReceivers,CanEmit,CanReceive,Shortest);
    //        MaxNarrowWeight/=1000;
    //        return HasTraced;
    //    }

    bool TraceWithinPatch(const size_t &IndexPatch,
                          const size_t Step,
                          bool AllReceivers,
                          bool ReduceMaxD)
    {
        assert(Step>=0);
        assert(Step<3);

        std::vector<size_t> PatchNodes;
        GetPatchNodes(IndexPatch,PatchNodes);
        std::set<size_t> PatchNodesSet(PatchNodes.begin(),PatchNodes.end());

        std::vector<size_t> FlatEmitters,FlatReceivers,ChosenEmitters,
                ChosenReceivers,FlatTangent,ChosenTangent;

        GetPatchBorderNodes(IndexPatch,FlatEmitters,FlatReceivers,ChosenEmitters,
                            ChosenReceivers,FlatTangent,ChosenTangent);

        //1 disable all but the one in the patch
        std::vector<bool> MustDisable(VFGraph.NumNodes(),true);
        for (size_t i=0;i<PatchNodes.size();i++)
            MustDisable[PatchNodes[i]]=false;

        //2: disable all corners (only if not in AllReceiverMode)
        if(!AllReceivers)
            for (size_t i=0;i<PartitionCorners[IndexPatch].size();i++)
            {
                std::vector<size_t> NodeI;
                VertexFieldGraph<MeshType>::IndexNodes(PartitionCorners[IndexPatch][i],NodeI);
                for (size_t j=0;j<NodeI.size();j++)
                    MustDisable[NodeI[j]]=true;
            }

        //3: disable all the tangent nodes to paths (only if not in AllReceiverMode)
        if (!AllReceivers)
        {
            for (size_t i=0;i<FlatTangent.size();i++)
                MustDisable[FlatTangent[i]]=true;

            for (size_t i=0;i<ChosenTangent.size();i++)
                MustDisable[ChosenTangent[i]]=true;
        }

        std::vector<bool> CanReceive(VFGraph.NumNodes(),false);
        for (size_t i=0;i<FlatReceivers.size();i++)
            CanReceive[FlatReceivers[i]]=true;
        for (size_t i=0;i<ChosenReceivers.size();i++)
            CanReceive[ChosenReceivers[i]]=true;
        if (AllReceivers)
        {
            for (size_t i=0;i<FlatTangent.size();i++)
                CanReceive[FlatTangent[i]]=true;
            for (size_t i=0;i<ChosenTangent.size();i++)
                CanReceive[ChosenTangent[i]]=true;

            if ((Step==0)||(Step==1))
            {
                for (size_t i=0;i<FlatEmitters.size();i++)
                    CanReceive[FlatEmitters[i]]=true;
                for (size_t i=0;i<ChosenEmitters.size();i++)
                    CanReceive[ChosenEmitters[i]]=true;
            }
        }
        //5: then get the emitters
        std::vector<bool> CanEmit(VFGraph.NumNodes(),false);
        std::vector<size_t> EmitterNodes;

        if (Step==0)
        {
            GetEmitterType(Narrow,EmitterNodes);
            EmitterNodes=FilterFromSet(EmitterNodes,PatchNodesSet);
            for (size_t i=0;i<EmitterNodes.size();i++)
                MustDisable[EmitterNodes[i]]=false;
        }
        if (Step==1)
        {
            std::cout<<"**** STEP 1"<<std::endl;
            GetEmitterType(Concave,EmitterNodes);
            EmitterNodes=FilterFromSet(EmitterNodes,PatchNodesSet);
            for (size_t i=0;i<EmitterNodes.size();i++)
                MustDisable[EmitterNodes[i]]=false;
            std::cout<<"END STEP 1"<<std::endl;
        }
        if (Step==2)
        {
            EmitterNodes=FlatEmitters;
            EmitterNodes.insert(EmitterNodes.end(),ChosenEmitters.begin(),ChosenEmitters.end());
        }

        for (size_t i=0;i<EmitterNodes.size();i++)
        {
            //if (PatchNodesSet.count(EmitterNodes[i])==0)continue;
            CanEmit[EmitterNodes[i]]=true;
            //only for flat ones
            MustDisable[EmitterNodes[i]]=false;
        }

        //6: do the trace
        VFGraph.SetAllActive();
        VFGraph.SetDisabledNodes(MustDisable);
        bool HasTraced=false;

        TypeVert FromType=Choosen;

        if (Step==0)
            FromType=Narrow;
        if (Step==1)
            FromType=Concave;

        //MaxNarrowWeight*=1000;
        if (ReduceMaxD)MaxNarrowWeight/=100;
        HasTraced=TraceFrom(FromType,Choosen,DijkstraReceivers,CanEmit,CanReceive,Shortest,
                            AllReceivers);
        if (ReduceMaxD)MaxNarrowWeight*=100;
        //MaxNarrowWeight/=1000;

        return HasTraced;
    }

    void GetMeshPatch(const size_t &IndexPatch,MeshType &PatchMesh)
    {
        vcg::tri::UpdateSelection<MeshType>::Clear(Mesh());

        for (size_t i=0;i<Partitions[IndexPatch].size();i++)
            Mesh().face[Partitions[IndexPatch][i]].SetS();

        //        for (size_t i=0;i<mesh.vert.size();i++)
        //            mesh.vert[i].Q()=i;
        //        for (size_t i=0;i<mesh.face.size();i++)
        //            mesh.face[i].Q()=i;

        vcg::tri::UpdateSelection<MeshType>::VertexFromFaceLoose(Mesh());

        PatchMesh.Clear();
        vcg::tri::Append<MeshType,MeshType>::Mesh(PatchMesh,Mesh(),true);
        PatchMesh.UpdateAttributes();
    }


    bool TraceWithinPatch(const size_t &IndexPatch)
    {
        bool traced;
        traced=TraceWithinPatch(IndexPatch,0,false,false);
        if (traced)return true;
        traced=TraceWithinPatch(IndexPatch,1,false,false);
        if (traced)return true;
        traced=TraceWithinPatch(IndexPatch,0,true,true);
        if (traced)return true;
        traced|=TraceWithinPatch(IndexPatch,1,true,true);
        if (traced)return true;

        traced=TraceWithinPatch(IndexPatch,2,false,false);
        if (traced)return true;
        std::cout<<"****** ALL RECEIVE MODE *******"<<std::endl;
        traced=TraceWithinPatch(IndexPatch,0,true,false);
        if (traced)return true;
        traced=TraceWithinPatch(IndexPatch,1,true,false);
        return traced;
        //        if (traced)return true;
        //        return false;
    }

    bool SolveSubPatchesStep()
    {
        std::cout<<"****** SUB PATCH TRACING *******"<<std::endl;

        bool HasTraced=false;
        do
        {
            HasTraced=false;
            RetrievePartitionsFromChoosen();
            for (size_t i=0;i<PartitionType.size();i++)
            {
                if (PartitionType[i]!=IsOK)
                {
                    HasTraced|=TraceWithinPatch(i);
                    //return true;
                }
                if (HasTraced)break;
            }
        }while (HasTraced);
        for (size_t i=0;i<PartitionType.size();i++)
            if (PartitionType[i]!=IsOK)return false;
        return true;
    }

    void SmoothPatches(size_t Steps=3,ScalarType Damp=0.5)
    {
        MeshType TargetMesh;
        vcg::tri::Append<MeshType,MeshType>::Mesh(TargetMesh,Mesh());
        TargetMesh.UpdateAttributes();
        vcg::GridStaticPtr<FaceType,ScalarType> Gr;
        Gr.Set(TargetMesh.face.begin(),TargetMesh.face.end());

        for (size_t i=0;i<Mesh().vert.size();i++)
            if (Mesh().vert[i].IsB())Mesh().vert[i].SetS();


        //get paths
        std::vector<bool> IsLoop;
        GetCurrChosenIsLoop(IsLoop);

        //smooth the path
        for (size_t s=0;s<Steps;s++)
        {
            std::vector<CoordType> AvPos(Mesh().vert.size(),CoordType(0,0,0));
            std::vector<size_t> NumDiv(Mesh().vert.size(),0);

            //smooth path
            for (size_t i=0;i<ChoosenPaths.size();i++)
            {
                size_t Limit=ChoosenPaths[i].PathNodes.size();
                if (!IsLoop[i])Limit-=1;
                size_t sizeNodes=ChoosenPaths[i].PathNodes.size();
                for (size_t j=0;j<Limit;j++)
                {
                    size_t NodeI0=ChoosenPaths[i].PathNodes[j];
                    size_t NodeI1=ChoosenPaths[i].PathNodes[(j+1)%sizeNodes];
                    size_t VInd0=VertexFieldGraph<MeshType>::NodeVertI(NodeI0);
                    size_t VInd1=VertexFieldGraph<MeshType>::NodeVertI(NodeI1);

                    CoordType Pos0=Mesh().vert[VInd0].P();
                    CoordType Pos1=Mesh().vert[VInd1].P();
                    CoordType Dir=Pos0-Pos1;
                    Dir.Normalize();

                    if ((IsLoop[i])||(j>0))
                    {
                        AvPos[VInd0]+=Pos1;
                        NumDiv[VInd0]++;
                    }
                    if ((IsLoop[i])||((j+1)!=(sizeNodes-1)))
                    {
                        AvPos[VInd1]+=Pos0;
                        NumDiv[VInd1]++;
                    }
                }
            }
            for (size_t i=0;i<Mesh().vert.size();i++)
            {
                //fixed
                if (Mesh().vert[i].IsS())continue;
                //no contributes
                if (NumDiv[i]==0)continue;

                CoordType TargetPos=(AvPos[i]/(ScalarType)NumDiv[i]);
                Mesh().vert[i].P()=(Mesh().vert[i].P()*Damp) +
                        ((TargetPos)*(1-Damp));

                Mesh().vert[i].SetV();
            }
            //smooth the rest
            for (size_t i=0;i<Mesh().face.size();i++)
                for (size_t j=0;j<3;j++)
                {
                    size_t IndexV0=vcg::tri::Index(Mesh(),Mesh().face[i].V0(j));
                    size_t IndexV1=vcg::tri::Index(Mesh(),Mesh().face[i].V1(j));
                    CoordType Pos0=Mesh().vert[IndexV0].P();
                    CoordType Pos1=Mesh().vert[IndexV1].P();
                    if (!Mesh().vert[IndexV0].IsV())
                    {
                        AvPos[IndexV0]+=Pos1;
                        NumDiv[IndexV0]++;
                    }
                    if (!Mesh().vert[IndexV1].IsV())
                    {
                        AvPos[IndexV1]+=Pos0;
                        NumDiv[IndexV1]++;
                    }
                }
            for (size_t i=0;i<Mesh().vert.size();i++)
            {
                //fixed
                if (Mesh().vert[i].IsS())continue;
                if (Mesh().vert[i].IsV())continue;
                //no contributes
                if (NumDiv[i]==0)continue;
                Mesh().vert[i].P()=(Mesh().vert[i].P()*Damp) +
                        ((AvPos[i]/NumDiv[i])*(1-Damp));
            }

            //reproject everything
            for (size_t i=0;i<Mesh().vert.size();i++)
            {
                //fixed
                CoordType Pos;
                ScalarType MinD;
                vcg::tri::GetClosestFaceBase(TargetMesh,Gr,Mesh().vert[i].P(),Mesh().bbox.Diag(),MinD,Pos);
                if (Mesh().vert[i].IsS())continue;
                Mesh().vert[i].P()=Pos;
            }
        }
        vcg::tri::UpdateSelection<MeshType>::VertexClear(Mesh());
        Mesh().UpdateAttributes();
        //        //update field
        //        for (size_t i=0;i<Mesh().face.size();i++)
        //        {
        //            CoordType OldNorm=Mesh().face[i].PD1()^Mesh().face[i].PD2();
        //            OldNorm.Normalize();
        //            CoordType NewNorm=Mesh().face[i].N();
        //            vcg::Matrix33<ScalarType> M=vcg::RotationMatrix(OldNorm,NewNorm);
        //            Mesh().face[i].PD1()=M*Mesh().face[i].PD1();
        //            Mesh().face[i].PD2()=Mesh().face[i].N()^Mesh().face[i].PD1();
        //        }
        //        for (size_t i=0;i<Mesh().vert.size();i++)
        //        {
        //            CoordType OldNorm=Mesh().vert[i].PD1()^Mesh().vert[i].PD2();
        //            OldNorm.Normalize();
        //            CoordType NewNorm=Mesh().vert[i].N();
        //            vcg::Matrix33<ScalarType> M=vcg::RotationMatrix(OldNorm,NewNorm);
        //            Mesh().vert[i].PD1()=M*Mesh().vert[i].PD1();
        //            Mesh().vert[i].PD1().Normalize();
        //            Mesh().vert[i].PD2()=Mesh().vert[i].N()^Mesh().vert[i].PD1();
        //            Mesh().vert[i].PD2().Normalize();
        //        }
    }

    void JoinNarrow(bool UpdatePartition=true)
    {
        bool Joined=true;
        size_t NumPath0=ChoosenPaths.size();
        do
        {
            Joined=false;

            //SHORT ONES
            MaxNarrowWeight/=100;
            Joined|=JoinConnection(Narrow,Flat,DijkstraReceivers);
            MaxNarrowWeight*=100;

            //NARROW TO NARROW
            //std::cout<<"0a"<<std::endl;
            Joined|=JoinConnection(Narrow,Narrow,DijkstraReceivers);

            //std::cout<<"1a"<<std::endl;
            //NARROW TO CONCAVE
            Joined|=JoinConnection(Narrow,Concave,DijkstraReceivers);

            //std::cout<<"2a"<<std::endl;
            //NARROW TO FLAT
            //MaxNarrowWeight*=1000;
            Joined|=JoinConnection(Narrow,Flat,TraceDirect);
            Joined|=JoinConnection(Narrow,Flat,DijkstraReceivers);
            //MaxNarrowWeight/=1000;
            //std::cout<<"3a"<<std::endl;
            //            //NARROW TO TRACED
            //            Joined|=JoinConnection(Narrow,Choosen,TraceDirect);
            //            Joined|=JoinConnection(Narrow,Choosen,DijkstraReceivers);

            std::cout<<"Still "<<UnsatisfiedNum()<<" Non Connected"<<std::endl;
        }
        while (Joined);
        size_t NumPath1=ChoosenPaths.size();
        if (NumPath1==NumPath0)return;

        if(UpdatePartition)
        {
            RetrievePartitionsFromChoosen();
            ColorByPartitions();
        }
    }

    void JoinConcave(bool UpdatePartition=true)
    {
        bool Joined=true;
        size_t NumPath0=ChoosenPaths.size();
        do
        {
            Joined=false;
            MaxNarrowWeight/=100;
            Joined|=JoinConnection(Concave,Flat,DijkstraReceivers);
            MaxNarrowWeight*=100;
            //
            //CONCAVE TO CONCAVE
            Joined|=JoinConnection(Concave,Concave,DijkstraReceivers);

            //CONCAVE TO FLAT
            Joined|=JoinConnection(Concave,Flat,TraceDirect);
            Joined|=JoinConnection(Concave,Flat,DijkstraReceivers);

            //            //CONCAVE TO TRACED
            //            Joined|=JoinConnection(Concave,Choosen,TraceDirect);
            //            Joined|=JoinConnection(Concave,Choosen,DijkstraReceivers);

            std::cout<<"Still "<<UnsatisfiedNum()<<" Non Connected"<<std::endl;
        }
        while (Joined);
        size_t NumPath1=ChoosenPaths.size();
        if (NumPath1==NumPath0)return;

        if(UpdatePartition)
        {
            RetrievePartitionsFromChoosen();
            ColorByPartitions();
        }
    }

    void JoinBoundaries(bool UpdatePartition=true)
    {
        std::vector<bool> IsActive;
        VFGraph.IsActiveNodes(IsActive);

        bool Joined=true;
        size_t NumPath0=ChoosenPaths.size();
        do
        {
            Joined=false;
            Joined|=JoinConnection(Flat,Flat,TraceDirect);
            //Joined|=JoinConnection(Flat,Choosen,TraceDirect);
        }while (Joined);


        VFGraph.SetActiveNodes(IsActive);

        size_t NumPath1=ChoosenPaths.size();
        if (NumPath1==NumPath0)return;

        if(UpdatePartition)
        {
            RetrievePartitionsFromChoosen();
            ColorByPartitions();
        }
    }


    void TraceLoops(bool UpdatePartition=true)
    {
        size_t NumPath0=ChoosenPaths.size();

        Candidates.clear();

        std::vector<bool> CanEmit,CanReceive,MustDisable;
        GetConfiguration(Internal,Internal,TraceLoop,CanEmit,CanReceive,MustDisable);

        VFGraph.SetAllActive();
        VFGraph.SetDisabledNodes(MustDisable);

        std::cout<<"Adding Loops candidates"<<std::endl;
        for (size_t i=0;i<VFGraph.NumNodes();i++)
        {
            //not the same kind
            if (!CanEmit[i])continue;


            //should be active
            assert(VFGraph.IsActive(i));

            Candidates.push_back(CandidateTrace(Internal,Internal,TraceLoop,i));
        }

        std::cout<<"There are "<<Candidates.size()<<" Candidates "<<std::endl;
        std::cout<<"Updating candidates"<<std::endl;
        UpdateCandidates(CanReceive);

        std::cout<<"Before Expansion there are "<<Candidates.size()<<" candidates"<<std::endl;
        ExpandCandidates();

        int size0=ChoosenPaths.size();
        std::cout<<"There are "<<Candidates.size()<<" candidates"<<std::endl;
        ChooseGreedyByDistance(false);
        int size1=ChoosenPaths.size();
        std::cout<<"Choosen "<<size1-size0<<std::endl;


        std::cout<<"Still "<<UnsatisfiedNum()<<" Non Connected"<<std::endl;
        size_t NumPath1=ChoosenPaths.size();
        if (NumPath1==NumPath0)return;

        //        RetrievePartitionsFromChoosen();
        //        ColorByPartitions();

        //        InvalidateTracedNodes();

        if(UpdatePartition)
        {
            RetrievePartitionsFromChoosen();
            ColorByPartitions();
        }
    }

    void BatchRemoval()
    {
        SmoothPatches(10);
        RemovePaths(false);
        if (!split_on_removal){
            FixValences();
            WriteInfo();
            return;
        }
        SplitIntoSubPaths();
        RemovePaths(false);
        FixValences();
        WriteInfo();
    }


    void FixCorners(size_t IndexPatch)
    {

        //save the original vertex index
        for (size_t i=0;i<Mesh().vert.size();i++)
            Mesh().vert[i].Q()=i;

        //extract the submesh
        vcg::tri::UpdateSelection<MeshType>::FaceClear(Mesh());
        vcg::tri::UpdateSelection<MeshType>::VertexClear(Mesh());

        for (size_t i=0;i<Partitions[IndexPatch].size();i++)
            Mesh().face[Partitions[IndexPatch][i]].SetS();

        vcg::tri::UpdateSelection<MeshType>::VertexFromFaceLoose(Mesh());
        MeshType TestMesh;
        vcg::tri::Append<MeshType,MeshType>::Mesh(TestMesh,Mesh(),true);
        TestMesh.UpdateAttributes();

        std::set<size_t> Corners(PartitionCorners[IndexPatch].begin(),
                                 PartitionCorners[IndexPatch].end());

        //compute sum angle
        std::vector<ScalarType > angleSumH(TestMesh.vert.size(),0);

        for(size_t i=0;i<TestMesh.face.size();i++)
        {
            for(int j=0;j<TestMesh.face[i].VN();++j)
            {
                size_t IndexV=vcg::tri::Index(TestMesh,TestMesh.face[i].V(j));
                angleSumH[IndexV] += face::WedgeAngleRad(TestMesh.face[i],j);
            }
        }

        std::vector<std::pair<ScalarType,size_t> > AngleVert;

        for(size_t i=0;i<TestMesh.vert.size();i++)
        {
            if(!TestMesh.vert[i].IsB())continue;
            AngleVert.push_back(std::pair<ScalarType,size_t>(angleSumH[i],TestMesh.vert[i].Q()));
        }
        std::sort(AngleVert.begin(),AngleVert.end());
        assert(AngleVert.size()>=3);

        if (Corners.size()<MinVal)
        {
            for (size_t i=0;i<AngleVert.size();i++)
            {
                if (Corners.count(AngleVert[i].second)>0)continue;
                Corners.insert(AngleVert[i].second);
                if (Corners.size()==MinVal)break;
            }
            assert(Corners.size()==MinVal);
        }
        else
        {
            assert(Corners.size()>MaxVal);
            std::reverse(AngleVert.begin(),AngleVert.end());
            for (size_t i=0;i<AngleVert.size();i++)
            {
                int IndexV=AngleVert[i].second;
                if (Corners.count(IndexV)==0)continue;
                if ((VertType[IndexV]==Narrow)
                        ||(VertType[IndexV]==Concave)
                        ||(VertType[IndexV]==Convex))continue;
                Corners.erase(IndexV);
                if (Corners.size()==MaxVal)break;
            }
            if (Corners.size()>MaxVal)
            {
                for (size_t i=0;i<AngleVert.size();i++)
                {
                    int IndexV=AngleVert[i].second;
                    if (Corners.count(IndexV)==0)continue;
                    Corners.erase(IndexV);
                    if (Corners.size()==MaxVal)break;
                }
            }
            assert(Corners.size()==MaxVal);
        }

        PartitionCorners[IndexPatch]=std::vector<size_t>(Corners.begin(),Corners.end());
    }

    void FixValences()
    {
        size_t NeedFix=0;
        for (size_t i=0;i<PartitionCorners.size();i++)
            if ((PartitionCorners[i].size()<MinVal)||
                    (PartitionCorners[i].size()>MaxVal))
            {
                NeedFix++;
                FixCorners(i);
            }
        std::cout<<"FINAL Fixed "<<NeedFix<<std::endl;
    }

    void BatchProcess(bool UpdatePartitions=true,
                      bool ForceReceivers=false)
    {
        //make a small tangent step
        if (ForceReceivers)
        {
            AllReceivers=true;
            MaxNarrowWeight/=100;
            JoinNarrow(false);
            JoinConcave(false);
            MaxNarrowWeight*=100;
            AllReceivers=false;
        }

        JoinNarrow(false);
        JoinConcave(false);

        TraceLoops(false);

        if (HasIncompleteEmitter())
            JoinNarrow(false);

        if (HasIncompleteEmitter())
            JoinConcave(false);

        //if (!HasTerminated())
        JoinBoundaries(false);

        if (HasIncompleteEmitter())
            JoinNarrow(false);

        if (HasIncompleteEmitter())
            JoinConcave(false);

        //        //if (!HasTerminated())
        //        TraceChoosen(false);

        //        if (HasIncompleteEmitter())
        //            JoinNarrow(false);
        //        if (HasIncompleteEmitter())
        //            JoinConcave(false);
        //SolveSubPatchesStep();

        if (ForceReceivers)
        {
            AllReceivers=true;
            if (HasIncompleteEmitter())
                JoinNarrow(false);

            if (HasIncompleteEmitter())
                JoinConcave(false);
            AllReceivers=false;
        }

        if (UpdatePartitions)
        {
            RetrievePartitionsFromChoosen();
            ColorByPartitions();
            WriteInfo();
        }
        //TraceLoops();

    }

    void  GetCurrCandidates(std::vector<std::vector<size_t> > &CurrCandidates)
    {
        CurrCandidates.clear();
        for (size_t i=0;i<Candidates.size();i++)
            CurrCandidates.push_back(Candidates[i].PathNodes);
    }

    void  GetCurrCandidatesIsLoop(std::vector<bool> &CurrCandidatesIsLoop)
    {
        CurrCandidatesIsLoop.clear();
        for (size_t i=0;i<Candidates.size();i++)
            CurrCandidatesIsLoop.push_back(Candidates[i].IsLoop);
    }

    void  GetCurrChosen(std::vector<std::vector<size_t> > &CurrChosen)
    {
        CurrChosen.clear();
        for (size_t i=0;i<ChoosenPaths.size();i++)
            CurrChosen.push_back(ChoosenPaths[i].PathNodes);
    }

    void  GetCurrVertDir(std::vector<std::vector<size_t> > &CurrV,
                         std::vector<std::vector<size_t> > &CurrDir,
                         std::vector<bool> &IsLoop)
    {
        CurrV.clear();
        CurrDir.clear();
        IsLoop.clear();
        CurrV.resize(ChoosenPaths.size());
        CurrDir.resize(ChoosenPaths.size());
        for (size_t i=0;i<ChoosenPaths.size();i++)
        {
            VFGraph.NodeVertI(ChoosenPaths[i].PathNodes,CurrV[i]);
            VFGraph.NodeDirI(ChoosenPaths[i].PathNodes,CurrDir[i]);
            IsLoop.push_back(ChoosenPaths[i].IsLoop);
        }
    }

    //    void  GetChosenInfo(
    //                        std::vector<bool > &IsLoop,
    //                        std::vector<TypeVert> &FromType,
    //                        std::vector<TypeVert> &ToType,
    //                        std::vector<TraceType> &TracingMethod,
    //                        std::vector<size_t> &InitNode)
    //    {
    //        IsLoop.clear();
    //        FromType.clear();
    //        ToType.clear();
    //        TracingMethod.clear();
    //        InitNode.clear();
    //        CurrChosen.clear();
    //        for (size_t i=0;i<ChoosenPaths.size();i++)
    //        {
    //            CurrChosen.push_back(ChoosenPaths[i].PathNodes);
    //            FromType.push_back(ChoosenPaths[i].FromType);
    //            ToType.push_back(ChoosenPaths[i].ToType);
    //            TracingMethod.push_back(ChoosenPaths[i].TracingMethod);
    //            InitNode.push_back(ChoosenPaths[i].InitNode);
    //        }
    //    }

    void  GetCurrChosenIsLoop(std::vector<bool> &CurrChosenIsLoop)
    {
        CurrChosenIsLoop.clear();
        for (size_t i=0;i<ChoosenPaths.size();i++)
            CurrChosenIsLoop.push_back(ChoosenPaths[i].IsLoop);
    }

    void GetUnsatisfied(std::vector<size_t> &Remaining)
    {
        Remaining.clear();
        for (size_t i=0;i<VerticesNeeds.size();i++)
        {
            if (VerticesNeeds[i]==0)continue;
            //assert((VertType[i]==Narrow)||(VertType[i]==Concave));
            std::vector<size_t> NodesI;
            VertexFieldGraph<MeshType>::IndexNodes(i,NodesI);
            for (size_t j=0;j<NodesI.size();j++)
            {
                if (!VFGraph.IsActive(NodesI[j]))continue;
                if((NodeEmitterTypes[NodesI[j]]==Narrow)||
                        (NodeEmitterTypes[NodesI[j]]==Concave))
                {
                    Remaining.push_back(NodesI[j]);
                }
            }
        }
    }

    void GetUnsolvedPartitions(std::vector<std::vector<size_t> > &UnsolvedPartition,
                               std::vector<PatchType> &UnsolvedType)
    {
        UnsolvedPartition.clear();
        UnsolvedType.clear();

        RetrievePartitionsFromChoosen(true);
        for (size_t i=0;i<Partitions.size();i++)
        {
            if (PartitionType[i]==IsOK)continue;
            UnsolvedPartition.push_back(Partitions[i]);
            UnsolvedType.push_back(PartitionType[i]);
        }
    }

    void GetUnsolvedPartitionsIndex(std::vector<size_t > &UnsolvedPartitionIndex)
    {
        UnsolvedPartitionIndex.clear();

        for (size_t i=0;i<Partitions.size();i++)
        {
            if (PartitionType[i]==IsOK)continue;
            UnsolvedPartitionIndex.push_back(i);
        }
    }

    void SetChoosenFromVertDir(const std::vector<std::vector<size_t> > &VertIdx,
                               const std::vector<std::vector<size_t> > &VertDir,
                               const std::vector<bool> &IsLoop)
    {
        ChoosenPaths.clear();
        assert(VertIdx.size()==VertDir.size());
        assert(VertIdx.size()==IsLoop.size());

        for (size_t i=0;i<VertIdx.size();i++)
        {
            CandidateTrace CTrace;
            for (size_t j=0;j<VertIdx[i].size();j++)
            {
                size_t IndexV=VertIdx[i][j];
                size_t IndexDir=VertDir[i][j];
                size_t IndexN=VertexFieldGraph<MeshType>::IndexNode(IndexV,IndexDir);
                CTrace.PathNodes.push_back(IndexN);
            }

            CTrace.FromType=VertType[VertIdx[i][0]];
            CTrace.ToType=VertType[VertIdx[i].back()];
            CTrace.TracingMethod=TraceDirect;
            CTrace.InitNode=CTrace.PathNodes[0];
            CTrace.IsLoop=IsLoop[i];
            CTrace.Updated=true;
            ChoosenPaths.push_back(CTrace);
        }
        UpdateVertNeedsFromChoosen();
        RetrievePartitionsFromChoosen();
        ColorByPartitions();
    }

    struct PathInfo
    {
        int PatchNum;
        int ValNum[10];
    };

    PatchTracer(VertexFieldGraph<MeshType> &_VFGraph):VFGraph(_VFGraph)
    {
        split_on_removal=false;
        avoid_increase_valence=true;
        avoid_collapse_irregular=false;
        max_lenght_distortion=1.2;
        max_lenght_variance=-1;
        sample_ratio=0.2;
        MinVal=3;
        MaxVal=6;
        AllReceivers=false;
    }
};

// Basic subdivision class
template<class MESH_TYPE>
struct SplitLev : public   std::unary_function<vcg::face::Pos<typename MESH_TYPE::FaceType> ,  typename MESH_TYPE::CoordType >
{
    typedef typename MESH_TYPE::VertexType VertexType;
    typedef typename MESH_TYPE::FaceType FaceType;
    typedef typename MESH_TYPE::CoordType CoordType;
    typedef typename MESH_TYPE::ScalarType ScalarType;

    typedef std::pair<CoordType,CoordType> EdgeCoordKey;

    std::map<EdgeCoordKey,CoordType> *SplitOps;

    void operator()(typename MESH_TYPE::VertexType &nv,vcg::face::Pos<typename MESH_TYPE::FaceType>  ep)
    {
        VertexType* v0=ep.f->V0(ep.z);
        VertexType* v1=ep.f->V1(ep.z);

        assert(v0!=v1);

        CoordType Pos0=v0->P();
        CoordType Pos1=v1->P();

        EdgeCoordKey CoordK(std::min(Pos0,Pos1),std::max(Pos0,Pos1));

        assert(SplitOps->count(CoordK)>0);
        nv.P()=(*SplitOps)[CoordK];
    }

    vcg::TexCoord2<ScalarType> WedgeInterp(vcg::TexCoord2<ScalarType> &t0,
                                           vcg::TexCoord2<ScalarType> &t1)
    {
        (void)t0;
        (void)t1;
        return (vcg::TexCoord2<ScalarType>(0,0));
    }

    SplitLev(std::map<EdgeCoordKey,CoordType> *_SplitOps){SplitOps=_SplitOps;}
    //SplitLevQ(){}
};

template <class MESH_TYPE>
class EdgePred
{
    typedef typename MESH_TYPE::VertexType VertexType;
    typedef typename MESH_TYPE::FaceType FaceType;
    typedef typename MESH_TYPE::CoordType CoordType;
    typedef typename MESH_TYPE::ScalarType ScalarType;
    typedef std::pair<CoordType,CoordType> EdgeCoordKey;

    std::map<EdgeCoordKey,CoordType> *SplitOps;

public:

    bool operator()(vcg::face::Pos<typename MESH_TYPE::FaceType> ep) const
    {
        VertexType* v0=ep.f->V0(ep.z);
        VertexType* v1=ep.f->V1(ep.z);

        assert(v0!=v1);

        CoordType Pos0=v0->P();
        CoordType Pos1=v1->P();

        EdgeCoordKey CoordK(std::min(Pos0,Pos1),std::max(Pos0,Pos1));

        return (SplitOps->count(CoordK)>0);
    }

    EdgePred(std::map<EdgeCoordKey,CoordType> *_SplitOps){SplitOps=_SplitOps;}
};

template <class MeshType>
bool TraceSubPatch(const size_t &IndexPatch,
                   PatchTracer<MeshType> &PTr,
                   std::vector<std::vector<size_t> > &VertIdx,
                   std::vector<std::vector<size_t> > &VertDir,
                   std::vector<bool> &IsLoop)
{
    //first copy the submesh
    for (size_t i=0;i<PTr.Mesh().vert.size();i++)
        PTr.Mesh().vert[i].Q()=i;

    MeshType SubMesh;
    PTr.GetMeshPatch(IndexPatch,SubMesh);
    SubMesh.UpdateAttributes();

    std::vector<size_t> VertMap;
    for (size_t i=0;i<SubMesh.vert.size();i++)
        VertMap.push_back(SubMesh.vert[i].Q());

    //copy original normals
    for (size_t i=0;i<SubMesh.vert.size();i++)
        SubMesh.vert[i].N()=PTr.Mesh().vert[VertMap[i]].N();

    //make a subgraph
    VertexFieldGraph<MeshType> VFGraph(SubMesh);
    VFGraph.Init();

    //initialize the tracer
    PatchTracer<MeshType> SubTr(VFGraph);
    //SubTr.Init(PTr.Drift);
    SubTr.CopyFrom(PTr,VertMap,IndexPatch);

    //then trace in the subpatch
    SubTr.BatchProcess(false,true);

    //copy back paths to the original
    SubTr.GetCurrVertDir(VertIdx,VertDir,IsLoop);
    for (size_t i=0;i<VertIdx.size();i++)
        for (size_t j=0;j<VertIdx[i].size();j++)
            VertIdx[i][j]=VertMap[VertIdx[i][j]];

    return (VertIdx.size()>0);
}

template <class MeshType>
void RecursiveProcess2(PatchTracer<MeshType> &PTr,
                       const typename MeshType::ScalarType Drift)
{
    //do a first step of tracing
    PTr.Init(Drift);
    PTr.BatchProcess();
    PTr.RetrievePartitionsFromChoosen(true);

    std::vector<std::vector<size_t> > TotVertIdx;
    std::vector<std::vector<size_t> > TotVertDir;
    std::vector<bool> TotIsLoop;
    PTr.GetCurrVertDir(TotVertIdx,TotVertDir,TotIsLoop);

    bool solved=false;
    bool traced=false;
    do{
        std::vector<size_t> UnsolvedPartitionIndex;
        //std::vector<PatchType> UnsolvedType;
        //PTr.GetUnsolvedPartitions(UnsolvedPartitions,UnsolvedType);
        PTr.GetUnsolvedPartitionsIndex(UnsolvedPartitionIndex);
        //PTr.GetUnsolvedPartitions(UnsolvedPartitions,UnsolvedType);
        if (UnsolvedPartitionIndex.size()==0)
            solved=true;

        traced=false;
        std::cout<<"**** THERE ARE "<<UnsolvedPartitionIndex.size()<<" Unsolved Partitions ****"<<std::endl;
        for(size_t i=0;i<UnsolvedPartitionIndex.size();i++)
        {

            std::vector<std::vector<size_t> > NewVertIdx;
            std::vector<std::vector<size_t> > NewVertDir;
            std::vector<bool> NewIsLoop;

            std::cout<<"**** SUB PATCH STEP ****"<<std::endl;
            traced|=TraceSubPatch<MeshType>(UnsolvedPartitionIndex[i],PTr,NewVertIdx,NewVertDir,NewIsLoop);

            if (traced)
                std::cout<<"TRACED"<<std::endl;
            else
                std::cout<<"NON TRACED"<<std::endl;

            TotVertIdx.insert(TotVertIdx.end(),NewVertIdx.begin(),NewVertIdx.end());
            TotVertDir.insert(TotVertDir.end(),NewVertDir.begin(),NewVertDir.end());
            TotIsLoop.insert(TotIsLoop.end(),NewIsLoop.begin(),NewIsLoop.end());

        }
        if (traced)
        {
            std::cout<<"Updating Patches"<<std::endl;
            PTr.SetChoosenFromVertDir(TotVertIdx,TotVertDir,TotIsLoop);
            std::cout<<"Done Updating Patches"<<std::endl;
        }

    }while(traced & (!solved));

    std::vector<size_t> UnsolvedPartitionIndex;
    PTr.GetUnsolvedPartitionsIndex(UnsolvedPartitionIndex);
    std::cout<<"**** FINAL THERE ARE "<<UnsolvedPartitionIndex.size()<<" Unsolved Partitions ****"<<std::endl;

    PTr.WriteInfo();
    PTr.FixValences();
    PTr.WriteInfo();
}

template <class MeshType>
void RefineStep(MeshType &mesh)
{
    typedef typename MeshType::VertexType VertexType;
    typedef typename MeshType::FaceType FaceType;
    typedef typename MeshType::CoordType CoordType;
    typedef typename MeshType::ScalarType ScalarType;
    typedef std::pair<CoordType,CoordType> EdgeCoordKey;

    //first mark the ones that must be splitted
    std::map<EdgeCoordKey,CoordType> SplitOps;

    mesh.SelectSharpFeatures();
    std::vector<std::pair<CoordType,CoordType> > SharpCoords;
    //for each face
    for (size_t i=0;i<mesh.face.size();i++)
    {
        //for each edge
        for (size_t j=0;j<3;j++)
        {
            CoordType Pos0=mesh.face[i].P0(j);
            CoordType Pos1=mesh.face[i].P1(j);
            EdgeCoordKey CoordK(std::min(Pos0,Pos1),std::max(Pos0,Pos1));
            CoordType NewPos=(Pos0+Pos1)/2;
            if (mesh.face[i].IsFaceEdgeS(j))
            {
                SharpCoords.push_back(std::pair<CoordType,CoordType>(std::min(Pos0,NewPos),
                                                                     std::max(Pos0,NewPos)));
                SharpCoords.push_back(std::pair<CoordType,CoordType>(std::min(Pos1,NewPos),
                                                                     std::max(Pos1,NewPos)));
            }
            //see if it has been already marked as cut
            if (SplitOps.count(CoordK)>0)continue;

            SplitOps[CoordK]=NewPos;
        }
    }

    std::sort(SharpCoords.begin(),SharpCoords.end());
    auto last=std::unique(SharpCoords.begin(),SharpCoords.end());
    SharpCoords.erase(last, SharpCoords.end());

    //UpdateFromCoordPairs(const std::vector<std::pair<CoordType,CoordType> > &SharpCoords)

    //if (SplitOps.size()==0)return;

    std::cout<<"Performing "<<SplitOps.size()<< " split ops"<<std::endl;
    SplitLev<MeshType> splMd(&SplitOps);
    EdgePred<MeshType> eP(&SplitOps);

    //do the final split
    bool done=vcg::tri::RefineE<MeshType,SplitLev<MeshType>,EdgePred<MeshType> >(mesh,splMd,eP);
    mesh.UpdateFromCoordPairs(SharpCoords);

    //return done;
}

template <class MeshType>
void UpdateTangentDirections(MeshType &mesh)
{
    typedef typename MeshType::CoordType CoordType;
    typedef typename MeshType::ScalarType ScalarType;

    //update field
    for (size_t i=0;i<mesh.face.size();i++)
    {
        CoordType OldNorm=mesh.face[i].PD1()^mesh.face[i].PD2();
        OldNorm.Normalize();
        CoordType NewNorm=mesh.face[i].N();
        vcg::Matrix33<ScalarType> M=vcg::RotationMatrix(OldNorm,NewNorm);
        mesh.face[i].PD1()=M*mesh.face[i].PD1();
        mesh.face[i].PD2()=mesh.face[i].N()^mesh.face[i].PD1();
    }
    for (size_t i=0;i<mesh.vert.size();i++)
    {
        CoordType OldNorm=mesh.vert[i].PD1()^mesh.vert[i].PD2();
        OldNorm.Normalize();
        CoordType NewNorm=mesh.vert[i].N();
        vcg::Matrix33<ScalarType> M=vcg::RotationMatrix(OldNorm,NewNorm);
        mesh.vert[i].PD1()=M*mesh.vert[i].PD1();
        mesh.vert[i].PD1().Normalize();
        mesh.vert[i].PD2()=mesh.vert[i].N()^mesh.vert[i].PD1();
        mesh.vert[i].PD2().Normalize();
    }
}

template <class MeshType>
bool FindPaths(MeshType &mesh,
               std::vector<size_t> &ValidFaces,
               const typename MeshType::ScalarType Drift,
               const typename MeshType::ScalarType Sample_Rate,
               std::vector<std::vector<size_t> > &VertIdx,
               std::vector<std::vector<size_t> > &VertDir,
               std::vector<bool> &IsLoop,
               std::vector<std::vector<size_t> > &UnsolvedPartitions,
               std::vector<PatchType> &UnsolvedType)
{
    VertIdx.clear();
    VertDir.clear();
    UnsolvedPartitions.clear();
    UnsolvedType.clear();

    vcg::tri::UpdateSelection<MeshType>::Clear(mesh);
    for (size_t i=0;i<ValidFaces.size();i++)
        mesh.face[ValidFaces[i]].SetS();

    for (size_t i=0;i<mesh.vert.size();i++)
        mesh.vert[i].Q()=i;
    for (size_t i=0;i<mesh.face.size();i++)
        mesh.face[i].Q()=i;

    vcg::tri::UpdateSelection<MeshType>::VertexFromFaceLoose(mesh);

    MeshType curr_mesh;
    vcg::tri::Append<MeshType,MeshType>::Mesh(curr_mesh,mesh,true);
    curr_mesh.UpdateAttributes();
    int num_splitted=vcg::tri::Clean<MeshType>::SplitNonManifoldVertex(curr_mesh,0);
    if (num_splitted>0)
        curr_mesh.UpdateAttributes();

    //save indexes
    std::vector<size_t> OriginalV,OriginalF;
    for (size_t i=0;i<curr_mesh.vert.size();i++)
        OriginalV.push_back(curr_mesh.vert[i].Q());

    for (size_t i=0;i<curr_mesh.face.size();i++)
        OriginalF.push_back(curr_mesh.face[i].Q());

    //copy old normals
    for (size_t i=0;i<curr_mesh.vert.size();i++)
    {
        size_t OrIndex=OriginalV[i];
        curr_mesh.vert[i].N()=mesh.vert[OrIndex].N();
    }

    //then update the tangent
    VertexFieldGraph<MeshType> VGraph(curr_mesh);
    VGraph.Init();
    PatchTracer<MeshType> PTr(VGraph);
    PTr.sample_ratio=Sample_Rate;
    PTr.Init(Drift);
    PTr.BatchProcess();

    //smooth
    PTr.GetCurrVertDir(VertIdx,VertDir,IsLoop);
    for (size_t i=0;i<VertIdx.size();i++)
        for (size_t j=0;j<VertIdx[i].size();j++)
            VertIdx[i][j]=OriginalV[VertIdx[i][j]];

    //    if (VertIdx.size()>0)
    //        PTr.SmoothPatches(10);

    //copy smooth on the original
    for (size_t i=0;i<curr_mesh.vert.size();i++)
    {
        size_t OrIndex=OriginalV[i];
        mesh.vert[OrIndex].P()=curr_mesh.vert[i].P();
        mesh.vert[OrIndex].PD1()=curr_mesh.vert[i].PD1();
        mesh.vert[OrIndex].PD2()=curr_mesh.vert[i].PD2();
    }

    //finally recover the unsolved faces
    PTr.GetUnsolvedPartitions(UnsolvedPartitions,UnsolvedType);

    //convert to original index
    for (size_t i=0;i<UnsolvedPartitions.size();i++)
        for (size_t j=0;j<UnsolvedPartitions[i].size();j++)
            UnsolvedPartitions[i][j]=OriginalF[UnsolvedPartitions[i][j]];

    return (VertIdx.size()>0);
}

#define MAX_ITERATION 8

template <class MeshType>
size_t RecursiveProcessGeoSmooth(PatchTracer<MeshType> &PTr,const typename MeshType::ScalarType Drift)
{
    std::vector<size_t> ValidFaces(PTr.Mesh().face.size(),0);
    for (size_t i=0;i<ValidFaces.size();i++)
        ValidFaces[i]=i;

    std::vector<std::vector<size_t> > TotVertIdx,TotVertDir;
    std::vector<bool> TotLoop;
    size_t performed_step=0;
    bool HasAdded=false;
    std::vector<std::vector<size_t> > TotUnsolvedPartitions;
    bool has_completed=false;
    do
    {
        std::vector<std::vector<size_t> > VertIdx,VertDir;
        std::vector<bool> IsLoop;
        std::vector<std::vector<size_t> > UnsolvedPartitions;
        std::vector<PatchType> UnsolvedType;
        FindPaths(PTr.Mesh(),ValidFaces,Drift,PTr.sample_ratio,
                  VertIdx,VertDir,IsLoop,UnsolvedPartitions,
                  UnsolvedType);

        TotVertIdx.insert(TotVertIdx.end(),VertIdx.begin(),VertIdx.end());
        TotVertDir.insert(TotVertDir.end(),VertDir.begin(),VertDir.end());
        TotLoop.insert(TotLoop.end(),IsLoop.begin(),IsLoop.end());

        HasAdded=(VertIdx.size()>0);
        //if added something then update
        if (HasAdded)
            TotUnsolvedPartitions.insert(TotUnsolvedPartitions.end(),
                                         UnsolvedPartitions.begin(),
                                         UnsolvedPartitions.end());

        performed_step++;
        std::cout<<"****** PERFORMED "<<performed_step<<" ******"<<std::endl;
        std::cout<<"* Added "<<VertIdx.size()<<" PATHS"<<std::endl;
        if (VertIdx.size()==1)
        {
            std::cout<<"* Size "<<VertIdx[0].size()<<" nodes"<<std::endl;
            std::cout<<"* V0 "<<VertIdx[0][0]<<std::endl;
            std::cout<<"* V1 "<<VertIdx[0][1]<<std::endl;
            size_t VIndex0=VertIdx[0][0];
            size_t VIndex1=VertIdx[0][1];
            std::cout<<"* Coord0 "<<PTr.Mesh().vert[VIndex0].P().X()
                    <<","<<PTr.Mesh().vert[VIndex0].P().Y()
                   <<","<<PTr.Mesh().vert[VIndex0].P().Z()<<std::endl;
            std::cout<<"* Coord1 "<<PTr.Mesh().vert[VIndex1].P().X()
                    <<","<<PTr.Mesh().vert[VIndex1].P().Y()
                   <<","<<PTr.Mesh().vert[VIndex1].P().Z()<<std::endl;
        }
        std::cout<<"* Unsolved "<<UnsolvedPartitions.size()<<" partitions "<<std::endl;
        if ((UnsolvedPartitions.size()==1)&&(VertIdx.size()==1))
        {
            std::cout<<"* Size "<<UnsolvedPartitions[0].size()<<" faces"<<std::endl;
            vcg::tri::UpdateSelection<MeshType>::FaceClear(PTr.Mesh());
            vcg::tri::UpdateSelection<MeshType>::VertexClear(PTr.Mesh());
            for(size_t i=0;i<UnsolvedPartitions[0].size();i++)
                PTr.Mesh().face[UnsolvedPartitions[0][i]].SetS();

            size_t VIndex0=VertIdx[0][0];
            size_t VIndex1=VertIdx[0][1];
            PTr.Mesh().vert[VIndex0].SetS();
            PTr.Mesh().vert[VIndex1].SetS();
            vcg::tri::io::ExporterPLY<MeshType>::Save(PTr.Mesh(),"test.ply",vcg::tri::io::Mask::IOM_FACEFLAGS|
                                                      vcg::tri::io::Mask::IOM_VERTFLAGS);
        }

        if (TotUnsolvedPartitions.size()>0)
        {
            ValidFaces=TotUnsolvedPartitions.back();
            TotUnsolvedPartitions.pop_back();
        }
        else has_completed=true;

    }while ((!has_completed)&&(performed_step<MAX_ITERATION));

    //finally reasseble the result in a single tracer
    PTr.Init(Drift);
    //PTr.WriteInfo();
    std::cout<<"****** REASSEMBLING ******"<<std::endl;
    PTr.SetChoosenFromVertDir(TotVertIdx,TotVertDir,TotLoop);
    PTr.WriteInfo();
    //PTr.FixValences();
    PTr.WriteInfo();
    return performed_step;
}

template <class MeshType>
void SaveCSV(PatchTracer<MeshType> &PTr,
             const std::string &pathProject,
             const size_t CurrNum)
{
    typename PatchTracer<MeshType>::PatchInfoType PInfo;
    PTr.GetInfo(PInfo);

    std::string pathCSVFinal=pathProject;
    pathCSVFinal=pathCSVFinal+"_p"+std::to_string(CurrNum)+".csv";
    FILE *f=fopen(pathCSVFinal.c_str(),"wt");
    assert(f!=NULL);
    fprintf(f,"%s %d %d %d %d %d %d %d %d %d %d %d %d %d \n",pathProject.c_str(),
            PInfo.NumPatchs,
            PInfo.HasEmit,
            PInfo.HighC,
            PInfo.LowC,
            PInfo.NonDiskLike,
            PInfo.SizePatches[0],
            PInfo.SizePatches[1],
            PInfo.SizePatches[2],
            PInfo.SizePatches[3],
            PInfo.SizePatches[4],
            PInfo.SizePatches[5],
            PInfo.SizePatches[6],
            PInfo.SizePatches[7]);
    fclose(f);
}


template <class MeshType>
void SaveAllData(PatchTracer<MeshType> &PTr,
                 const std::string &pathProject,
                 const size_t CurrNum)
{
    typedef typename MeshType::CoordType CoordType;

    std::vector<std::pair<CoordType,CoordType> > SharpCoords;
    PTr.Mesh().GetSharpCoordPairs(SharpCoords);

    //copy the mesh
    MeshType SaveM;
    vcg::tri::Append<MeshType,MeshType>::Mesh(SaveM,PTr.Mesh());
    std::vector<size_t> SharpCorners;
    PTr.getCornerSharp(SharpCorners);
    std::set<CoordType> SharpCornerPos;
    for(size_t i=0;i<SharpCorners.size();i++)
        SharpCornerPos.insert(PTr.Mesh().vert[SharpCorners[i]].P());

    //merge vertices
    vcg::tri::Clean<MeshType>::RemoveDuplicateVertex(SaveM);
    vcg::tri::Clean<MeshType>::RemoveUnreferencedVertex(SaveM);
    vcg::tri::Allocator<MeshType>::CompactEveryVector(SaveM);
    SaveM.UpdateAttributes();
    SaveM.UpdateFromCoordPairs(SharpCoords);

    //save vert pos
    std::map<CoordType,size_t> VertMap;
    for (size_t i=0;i<SaveM.vert.size();i++)
        VertMap[SaveM.vert[i].P()]=i;

    for (size_t i=0;i<SaveM.face.size();i++)
        SaveM.face[i].Q()=PTr.FacePartitions[i];

    RefineStep(SaveM);

    //update sharp vertices
    SaveM.SharpCorners.clear();
    for (size_t i=0;i<SaveM.vert.size();i++)
        if (SharpCornerPos.count(SaveM.vert[i].P())>0)
            SaveM.SharpCorners.push_back(i);

    //save the mesh
    int Mask=0;
    std::string pathMeshFinal=pathProject;
    pathMeshFinal=pathMeshFinal+"_p"+std::to_string(CurrNum)+".obj";
    vcg::tri::io::ExporterOBJ<MeshType>::Save(SaveM,pathMeshFinal.c_str(),Mask);

    std::string pathPartitions=pathProject;
    //pathPartitions.append("_p.patch");
    pathPartitions=pathPartitions+"_p"+std::to_string(CurrNum)+".patch";
    FILE *F=fopen(pathPartitions.c_str(),"wt");
    assert(F!=NULL);
    //    assert(PTr.FacePartitions.size()==PTr.Mesh().face.size());
    //    fprintf(F,"%d\n",PTr.FacePartitions.size());
    //    for (size_t i=0;i<PTr.FacePartitions.size();i++)
    //        fprintf(F,"%d\n",PTr.FacePartitions[i]);
    //    fclose(F);
    fprintf(F,"%d\n",SaveM.face.size());
    for (size_t i=0;i<SaveM.face.size();i++)
        fprintf(F,"%d\n",(int)SaveM.face[i].Q());
    fclose(F);

    std::string pathCorners=pathProject;
    //pathCorners.append("_p.corners");
    pathCorners=pathCorners+"_p"+std::to_string(CurrNum)+".corners";
    F=fopen(pathCorners.c_str(),"wt");
    assert(F!=NULL);
    assert(PTr.PartitionCorners.size()==PTr.Partitions.size());
    fprintf(F,"%d\n",PTr.PartitionCorners.size());
    for (size_t i=0;i<PTr.PartitionCorners.size();i++)
    {
        fprintf(F,"%d\n",PTr.PartitionCorners[i].size());
        for (size_t j=0;j<PTr.PartitionCorners[i].size();j++)
        {
            size_t IndexV=PTr.PartitionCorners[i][j];
            CoordType CornerPos=PTr.Mesh().vert[IndexV].P();
            assert(VertMap.count(CornerPos)>0);
            fprintf(F,"%d\n",VertMap[CornerPos]);
        }
    }
    fclose(F);

    std::string featurePartitions=pathProject;
    featurePartitions=featurePartitions+"_p"+std::to_string(CurrNum)+".feature";
    SaveM.SaveFeatures(featurePartitions);

    std::string featureCorners=pathProject;
    featureCorners=featureCorners+"_p"+std::to_string(CurrNum)+".c_feature";
    SaveM.SaveSharpCorners(featureCorners);
}

#endif
