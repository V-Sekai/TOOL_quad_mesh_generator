#ifndef GRAPH_QUERY
#define GRAPH_QUERY

template <class MeshType>
class VertexFieldQuery
{
    typedef typename MeshType::CoordType CoordType;
    typedef typename MeshType::ScalarType ScalarType;
    typedef typename MeshType::FaceType FaceType;
    typedef typename MeshType::VertexType VertexType;

    //follow the direction from one node to another
    static bool TraceNext(const VertexFieldGraph<MeshType> &VertGraph,
                          const size_t IndexNode0,
                          size_t &IndexNode1)
    {
        //return(VertGraph.FirstValid(IndexNode0,IndexNode1));
        if (VertGraph.NumNeigh(IndexNode0)==0)return false;
        IndexNode1=VertGraph.NodeNeigh(IndexNode0,0);
        return (VertGraph.IsActive(IndexNode1));
    }


public:

    static bool SelfIntersect(const VertexFieldGraph<MeshType> &VFGraph,
                              const std::vector<size_t > &Path,
                              const bool IsLoop)
    {
        std::set<std::pair<CoordType,CoordType> > PathEdges;
        size_t Limit=Path.size()-1;
        if (IsLoop)Limit++;
        for (size_t i=0;i<Limit;i++)
        {
            CoordType Pos0=VFGraph.NodePos(Path[i]);
            CoordType Pos1=VFGraph.NodePos(Path[(i+1)%Path.size()]);
            std::pair<CoordType,CoordType> Key(std::min(Pos0,Pos1),std::max(Pos0,Pos1));
            if (PathEdges.count(Key)>0)return true;
            PathEdges.insert(Key);
        }

        std::set<std::pair<size_t,size_t> > VertDir;
        for (size_t i=0;i<Path.size();i++)
        {
            size_t VertI=VertexFieldGraph<MeshType>::NodeVertI(Path[i]);
            size_t DirI=VertexFieldGraph<MeshType>::NodeDirI(Path[i]);

            std::pair<size_t,size_t> Key(VertI,DirI%2);
            if (VertDir.count(Key)>0)return true;
            VertDir.insert(Key);
        }
        return false;
    }

    static bool CollideSubSequence(const VertexFieldGraph<MeshType> &VFGraph,
                                   const std::vector<size_t > &VIndex0,
                                   const std::vector<size_t > &VIndex1)
    {
        assert(VIndex0.size()==3);
        assert(VIndex1.size()==3);
        if (VIndex0[1]!=VIndex1[1])return false;

        //get a first face
        VertexType *vMiddle=&VFGraph.Mesh().vert[VIndex0[1]];
        vcg::face::VFIterator<FaceType> VfI(vMiddle);
        FaceType *F0 = VfI.F();
        size_t IndexV= VfI.I();
        assert(F0->V(IndexV)==vMiddle);

        //initialize a pos
        vcg::face::Pos<FaceType> InitPos(F0,IndexV);
        std::vector<vcg::face::Pos<FaceType> > PosStar;
        vcg::face::VFOrderedStarFF(InitPos,PosStar);

        int IndexPartition=-1;
        for (size_t i=0;i<PosStar.size();i++)
        {
            assert(PosStar[i].V()==vMiddle);
            VertexType *vOpp=PosStar[i].VFlip();
            size_t IndexVOpp=vcg::tri::Index(VFGraph.Mesh(),vOpp);

            if ((IndexVOpp==VIndex0[0])||(IndexVOpp==VIndex0[2]))
            {
                if (IndexPartition==-1)
                    IndexPartition=0;
                else
                    if (IndexPartition==0)
                        return true;
                    else
                        if (IndexPartition==1)
                            IndexPartition=0;

            }

            if ((IndexVOpp==VIndex1[0])||(IndexVOpp==VIndex1[2]))
            {
                if (IndexPartition==-1)
                    IndexPartition=1;
                else
                    if (IndexPartition==1)
                        return true;
                    else
                        if (IndexPartition==0)
                            IndexPartition=1;
            }
        }
        return false;
        //vcg::face::Pos<FaceType> currPos(F0,IndexV);
    }

    //*** COLLIDING FUNCTIONS ***
    //return true if two traces collides
    static bool CollideTraces(const VertexFieldGraph<MeshType> &VFGraph,
                              const std::vector<size_t > &TraceN0,
                              const std::vector<size_t > &TraceN1,
                              bool IsLoopTr0,bool IsLoopTr1)
    {
        //quick rejection test based on vertex indexes
        std::set<CoordType> VIndexTrace0;
        for (size_t i=0;i<TraceN0.size();i++)
            VIndexTrace0.insert(VFGraph.NodePos(TraceN0[i]));

        bool HasSameV=false;
        for (size_t i=0;i<TraceN1.size();i++)
        {
            if(VIndexTrace0.count(VFGraph.NodePos(TraceN1[i]))==1)
            {
                HasSameV=true;
                break;
            }
        }
        if (!HasSameV)return false;

        //then check if there is the same geometric edge
        std::set<std::pair<CoordType,CoordType> > PathEdges0;
        size_t Limit0=TraceN0.size()-1;
        if (IsLoopTr0)Limit0++;
        for (size_t i=0;i<Limit0;i++)
        {
            CoordType Pos0=VFGraph.NodePos(TraceN0[i]);
            CoordType Pos1=VFGraph.NodePos(TraceN0[(i+1)%TraceN0.size()]);
            std::pair<CoordType,CoordType> Key(std::min(Pos0,Pos1),std::max(Pos0,Pos1));
            PathEdges0.insert(Key);
        }
        size_t Limit1=TraceN1.size()-1;
        if (IsLoopTr1)Limit1++;
        for (size_t i=0;i<Limit1;i++)
        {
            CoordType Pos0=VFGraph.NodePos(TraceN1[i]);
            CoordType Pos1=VFGraph.NodePos(TraceN1[(i+1)%TraceN1.size()]);
            std::pair<CoordType,CoordType> Key(std::min(Pos0,Pos1),std::max(Pos0,Pos1));
            if (PathEdges0.count(Key)>0)return true;
        }

        //first check if there is the same node (or opposite in M2)
        std::set<size_t> PathNodes0(TraceN0.begin(),TraceN0.end());

        //in this case add also the opposite
//        if (IsLoopTr0)
//        {
            for (size_t i=0;i<TraceN0.size();i++)
                PathNodes0.insert(VertexFieldGraph<MeshType>::TangentNode(TraceN0[i]));
//        }
//        else
//        {
//            //otherwise not the first and the last
//            for (size_t i=1;i<TraceN0.size()-1;i++)
//                PathNodes0.insert(VertexFieldGraph<MeshType>::TangentNode(TraceN0[i]));
//        }

        for (size_t i=0;i<TraceN1.size();i++)
        {
            size_t NodeN1=TraceN1[i];
            if (PathNodes0.count(NodeN1)>0)return true;

            //add the opposite of the first/last only when is loop
            //if ((!IsLoopTr0)&&((i==0)||(i==(TraceN1.size()-1))))continue;
            size_t NodeOppN1=VertexFieldGraph<MeshType>::TangentNode(NodeN1);
            if (PathNodes0.count(NodeOppN1)>0)return true;
        }

        //check strange propagation configuration that might happens
        //due to discretization
        size_t Start0=1;
        size_t Start1=1;
        if (IsLoopTr0)Start0=0;
        if (IsLoopTr1)Start1=0;
        size_t Size0=TraceN0.size();
        size_t Size1=TraceN1.size();
        for (size_t i=Start0;i<Limit0;i++)
        {
            //not check on twins or border
            if (VFGraph.IsBorder(TraceN0[i]))continue;
            std::vector<size_t> VIndex0;
            size_t I0=(i+Size0-1)%Size0;
            size_t I1=i;
            size_t I2=(i+1)%Size0;
            size_t IndexV0=VertexFieldGraph<MeshType>::NodeVertI(TraceN0[I0]);
            size_t IndexV1=VertexFieldGraph<MeshType>::NodeVertI(TraceN0[I1]);
            size_t IndexV2=VertexFieldGraph<MeshType>::NodeVertI(TraceN0[I2]);
            VIndex0.push_back(IndexV0);
            VIndex0.push_back(IndexV1);
            VIndex0.push_back(IndexV2);
            for (size_t j=Start1;j<Limit1;j++)
            {
                if (VFGraph.IsBorder(TraceN1[j]))continue;
                std::vector<size_t> VIndex1;
                size_t I0=(j+Size1-1)%Size1;
                size_t I1=j;
                size_t I2=(j+1)%Size1;
                size_t IndexV0=VertexFieldGraph<MeshType>::NodeVertI(TraceN1[I0]);
                size_t IndexV1=VertexFieldGraph<MeshType>::NodeVertI(TraceN1[I1]);
                size_t IndexV2=VertexFieldGraph<MeshType>::NodeVertI(TraceN1[I2]);
                VIndex1.push_back(IndexV0);
                VIndex1.push_back(IndexV1);
                VIndex1.push_back(IndexV2);
                if (CollideSubSequence(VFGraph,VIndex0,VIndex1))return true;
            }
        }

        return false;
    }


    //trace from one Index of Vertex toward a direction
    //it can be also tuned to stop when it encounter a selected vertex
    static bool TraceToSelected(VertexFieldGraph<MeshType> &VertGraph,
                                const size_t StartNode,
                                std::vector<size_t> &IndexNodes)
    {
        VertGraph.UnMarkAll();

        size_t CurrNode=StartNode;
        IndexNodes.clear();
        IndexNodes.push_back(StartNode);
        bool has_terminated=false;

        do {
            size_t TangentNode=VertexFieldGraph<MeshType>::TangentNode(CurrNode);
            assert(TangentNode!=CurrNode);

            //mark current and tangent nodes
            VertGraph.Mark(CurrNode);
            if (VertGraph.IsActive(TangentNode))
                VertGraph.Mark(TangentNode);

            size_t NextNode;
            bool traced=TraceNext(VertGraph,CurrNode,NextNode);
            if (!traced)
            {
                //std::cout<<"stopped tracing"<<std::endl;
                return false;
            }

            if (VertGraph.IsMarked(NextNode))
            {
                //std::cout<<"WARNING self intersecting in tracing (discarded)"<<std::endl;
                return false;
            }

            assert(NextNode!=CurrNode);
            CurrNode=NextNode;

            IndexNodes.push_back(CurrNode);

            has_terminated=VertGraph.IsSelected(CurrNode);

        }while(!has_terminated);
        return true;
    }



    struct HeapEntry
    {
        ScalarType Weight;
        size_t NodeI;

        bool operator < (const HeapEntry &He) const
        {
            if( Weight != He.Weight)
                return (Weight > He.Weight);
            return (NodeI<He.NodeI);
        }

        HeapEntry(size_t _NodeI,
                  ScalarType _Weight)
        {
            Weight=_Weight;
            NodeI=_NodeI;
        }
    };

    //        static void RetrievePath(VertexFieldGraph<MeshType> &VFGraph,
    //                                 size_t IndexN,std::vector<size_t> &PathN)
    //        {
    //            PathN.clear();
    //            size_t CurrN=IndexN;
    //            //retrieve the sequence
    //            do {
    //                PathN.push_back(CurrN);
    //                assert(VFGraph.IsMarked(CurrN));
    //                CurrN=VFGraph.Father(CurrN);
    //            }while ((CurrN!=VFGraph.Father(CurrN))||(CurrN==IndexN));

    //            //in case of loops no need to add again
    //            if (CurrN!=IndexN)
    //                PathN.push_back(CurrN);

    //            std::reverse(PathN.begin(),PathN.end());
    //        }

    static void RetrievePath(VertexFieldGraph<MeshType> &VFGraph,
                             size_t IndexN,std::vector<size_t> &PathN)
    {
        PathN.clear();
        size_t CurrN=IndexN;
        //retrieve the sequence
        do {
            PathN.push_back(CurrN);
            assert(VFGraph.IsMarked(CurrN));
            CurrN=VFGraph.Father(CurrN);
        }while ((int)CurrN!=VFGraph.Father(CurrN));

        //in case of loops no need to add again
        //if (CurrN!=IndexN)
        PathN.push_back(CurrN);

        std::reverse(PathN.begin(),PathN.end());
    }

    //    inline ScalarType geoDistance(const ScalarType &max_angle = 45,
    //                                  const ScalarType &drift_penalty = 2)
    //    {
    //        if (max_angle == ScalarType(0))
    //            return (length*Weight);
    //        //return length * (ScalarType(1) + drift_penalty * angle / max_angle)*Weight;
    //        ScalarType drift=pow(angle/max_angle,2);
    //        //ScalarType drift=angle/max_angle;
    ////            if (Weight<1)
    ////                std::cout<<"W "<<Weight<<std::endl;
    ////            else
    ////                std::cout<<"W 1"<<Weight<<std::endl;
    //        return length * (ScalarType(1) + drift_penalty * drift)*Weight;
    //    }

    static ScalarType Weight(const ScalarType Lenght,
                             const ScalarType Angle,
                             const ScalarType DriftPenalty,
                             ScalarType MaxAngle)
    {
        if (MaxAngle <= ScalarType(0))
            return (Lenght);

        ScalarType drift=pow(Angle/MaxAngle,2);
        return Lenght * (ScalarType(1) + DriftPenalty * drift);

    }

    //*** LENGHT FUNCTIONS ***
    //return the lenght of an edge, considering the field
    static ScalarType EdgeLengh(const VertexFieldGraph<MeshType> &VFGraph,
                                const size_t IndexN0,
                                const size_t IndexN1)
    {
        CoordType edgedir=(VFGraph.NodePos(IndexN0)-
                           VFGraph.NodePos(IndexN1));
        CoordType Dir=VFGraph.NodeDir(IndexN0);
        return(fabs(edgedir*Dir));
    }



    //return the lenght of an trace, considering the field
    static ScalarType TraceLenght(const VertexFieldGraph<MeshType> &VFGraph,
                                  std::vector<size_t> &TraceNodes,
                                  bool IsLoop)
    {
        ScalarType Len=0;
        size_t Limit=TraceNodes.size()-1;
        if(IsLoop)Limit++;
        for (size_t i=0;i<Limit;i++)
            Len+=EdgeLengh(VFGraph,TraceNodes[i],TraceNodes[(i+1)%TraceNodes.size()]);

        return Len;
    }

    //return the average distances of an trace
    static ScalarType TraceAVGDistance(const VertexFieldGraph<MeshType> &VFGraph,
                                       std::vector<size_t> &TraceNodes)
    {
        ScalarType Dist=0;
        for (size_t i=0;i<TraceNodes.size();i++)
            Dist+=VFGraph.Distance(TraceNodes[i]);

        return (Dist/TraceNodes.size());
    }

    struct ShortParam
    {
        std::vector<size_t> StartNode;
        ScalarType MaxAngle;
        ScalarType DriftPenalty;
        int MaxJump;
        int MaxTwin;
        ScalarType MaxWeight;
        bool OnlyDirect;
        bool StopAtSel;
        int TargetNode;
        bool LoopMode;
        bool AvoidBorder;
        ShortParam()
        {
            MaxAngle=45;
            DriftPenalty=10;
            MaxJump=-1;
            MaxTwin=-1;
            MaxWeight=-1;
            OnlyDirect=false;
            StopAtSel=true;
            TargetNode=-1;
            LoopMode=false;
            AvoidBorder=false;
        }
    };

    //    static bool ShortestPath(VertexFieldGraph<MeshType> &VFGraph,
    //                             const ShortParam& SParam,
    //                             std::vector<size_t> &PathN)
    //    {
    //        std::vector<HeapEntry> Heap;
    //        VFGraph.UnMarkAll();
    //        if (SParam.LoopMode)
    //        {
    //            assert(SParam.StartNode.size()==1);
    //            assert(SParam.StartNode[0]==SParam.TargetNode);
    //        }
    //        size_t VisitedSource=0;

    //        for (size_t i=0;i<SParam.StartNode.size();i++)
    //        {
    //            size_t IndexN0=SParam.StartNode[i];
    //            assert(VFGraph.IsActive(IndexN0));
    //            VFGraph.Mark(IndexN0);
    //            VFGraph.Distance(IndexN0)=0;
    //            VFGraph.Jumps(IndexN0)=0;
    //            VFGraph.Father(IndexN0)=IndexN0;
    //            VFGraph.TwinJumps(IndexN0)=0;
    //            Heap.push_back(HeapEntry(IndexN0,0));
    //        }

    //        //set initial
    //        std::make_heap(Heap.begin(),Heap.end());
    //        do
    //        {
    //            pop_heap(Heap.begin(),Heap.end());
    //            size_t CurrN=(Heap.back()).NodeI;
    //            ScalarType CurrWeight=(Heap.back()).Weight;
    //            Heap.pop_back();

    //            if (!SParam.LoopMode)
    //                assert(VFGraph.IsMarked(CurrN));
    //            else
    //            {
    //                if (CurrN!=SParam.TargetNode)
    //                    assert(VFGraph.IsMarked(CurrN));
    //                else
    //                    VisitedSource++;
    //            }

    //            //then had found the final step
    //            bool HasTerminated=(SParam.StopAtSel)&&(VFGraph.IsSelected(CurrN));
    //            HasTerminated|=(SParam.TargetNode==CurrN);

    ////            if (SParam.LoopMode)
    ////                HasTerminated&=(VisitedSource>1);


    //            //check minimal number of jumps (useful for loops)
    //            int CurrJump=VFGraph.Jumps(CurrN);
    ////            if (SParam.MinJump>0)
    ////                HasTerminated&=(CurrJump>=SParam.MinJump);

    //            if (HasTerminated)
    //            {
    //                RetrievePath(VFGraph,CurrN,PathN);
    //                return true;
    //            }

    //            size_t CurrTwin=VFGraph.TwinJumps(CurrN);

    //            if ((SParam.MaxTwin>0)&&(CurrTwin>SParam.MaxTwin))continue;

    //            //std::cout<<"test at "<<CurrJump<<std::endl;
    //            if ((SParam.MaxJump>0)&&(CurrJump==SParam.MaxJump))continue;//(pow(PropagationSteps,2)+1))continue;//maximum number of jumps reached

    //            if ((SParam.MaxWeight>0)&&(CurrWeight>SParam.MaxWeight))continue;

    //            for (size_t i=0;i<VFGraph.NumNeigh(CurrN);i++)
    //            {
    //                if ((SParam.OnlyDirect) && (!VFGraph.DirectNeigh(CurrN,i)))continue;
    //                if (!VFGraph.ActiveNeigh(CurrN,i))continue;

    //                size_t NextN=VFGraph.NodeNeigh(CurrN,i);

    //                bool IsTwin=VFGraph.TwinNeigh(CurrN,i);
    //                assert(CurrN!=NextN);

    //                if (!VFGraph.IsActive(NextN))continue;

    //                ScalarType AngleNeigh=VFGraph.AngleNeigh(CurrN,i);
    //                if ((SParam.MaxAngle>0)&&(AngleNeigh>SParam.MaxAngle))continue;

    //                ScalarType LenNeigh=VFGraph.DistNeigh(CurrN,i);

    //                ScalarType NextWeight=CurrWeight+Weight(LenNeigh,AngleNeigh,SParam.DriftPenalty,SParam.MaxAngle);

    //                bool AddHeap=((!VFGraph.IsMarked(NextN))||(VFGraph.Distance(NextN)>NextWeight));
    ////                if (!SParam.LoopMode)
    ////                    AddHeap|=(NextN==SParam.TargetNode);
    //                if (AddHeap)
    //                {
    //                    VFGraph.Mark(NextN);
    //                    VFGraph.Distance(NextN)=NextWeight;
    //                    VFGraph.Father(NextN)=CurrN;
    //                    VFGraph.Jumps(NextN)=CurrJump+1;
    //                    VFGraph.TwinJumps(NextN)=CurrTwin;
    //                    if (IsTwin)VFGraph.TwinJumps(NextN)++;
    //                    Heap.push_back(HeapEntry(NextN,NextWeight));
    //                    push_heap(Heap.begin(),Heap.end());
    //                }
    //            }
    //        }while (!Heap.empty());
    //        return false;
    //    }

    //    static bool ShortestPath(VertexFieldGraph<MeshType> &VFGraph,
    //                             const ShortParam& SParam,
    //                             std::vector<size_t> &PathN)
    //    {
    //        std::vector<HeapEntry> Heap;
    //        VFGraph.UnMarkAll();

    //        if (SParam.LoopMode)
    //        {
    //            assert(SParam.StartNode.size()==1);
    //            assert(SParam.StartNode[0]==SParam.TargetNode);
    //        }
    //        size_t VisitedSource=0;

    //        for (size_t i=0;i<SParam.StartNode.size();i++)
    //        {
    //            size_t IndexN0=SParam.StartNode[i];
    //            assert(VFGraph.IsActive(IndexN0));
    //            VFGraph.Mark(IndexN0);
    //            VFGraph.Distance(IndexN0)=0;
    //            VFGraph.Jumps(IndexN0)=0;
    //            VFGraph.Father(IndexN0)=IndexN0;
    //            VFGraph.TwinJumps(IndexN0)=0;
    //            Heap.push_back(HeapEntry(IndexN0,0));
    //        }

    //        //set initial
    //        std::make_heap(Heap.begin(),Heap.end());
    //        do
    //        {
    //            pop_heap(Heap.begin(),Heap.end());
    //            size_t CurrN=(Heap.back()).NodeI;
    //            ScalarType CurrWeight=(Heap.back()).Weight;
    //            Heap.pop_back();

    //            if (!SParam.LoopMode)
    //                assert(VFGraph.IsMarked(CurrN));
    //            else
    //            {
    //                if (CurrN!=SParam.TargetNode)
    //                    assert(VFGraph.IsMarked(CurrN));
    //                else
    //                {
    //                    VisitedSource++;
    //                    std::cout<<"DE "<<VisitedSource<<std::endl;
    //                }
    //            }

    //            //then had found the final step
    //            bool HasTerminated=(SParam.StopAtSel)&&(VFGraph.IsSelected(CurrN));
    //            HasTerminated|=(SParam.TargetNode==CurrN);

    //            if (SParam.LoopMode)
    //                HasTerminated&=(VisitedSource>1);


    //            //check minimal number of jumps (useful for loops)
    //            int CurrJump=VFGraph.Jumps(CurrN);
    //            //            if (SParam.MinJump>0)
    //            //                HasTerminated&=(CurrJump>=SParam.MinJump);

    //            if (HasTerminated)
    //            {
    //                std::cout<<"Retrieving"<<std::endl;
    //                RetrievePath(VFGraph,CurrN,PathN);
    //                std::cout<<"Done"<<std::endl;
    //                return true;
    //            }

    //            size_t CurrTwin=VFGraph.TwinJumps(CurrN);

    //            if ((SParam.MaxTwin>0)&&(CurrTwin>SParam.MaxTwin))continue;

    //            //std::cout<<"test at "<<CurrJump<<std::endl;
    //            if ((SParam.MaxJump>0)&&(CurrJump==SParam.MaxJump))continue;//(pow(PropagationSteps,2)+1))continue;//maximum number of jumps reached

    //            if ((SParam.MaxWeight>0)&&(CurrWeight>SParam.MaxWeight))continue;

    //            for (size_t i=0;i<VFGraph.NumNeigh(CurrN);i++)
    //            {
    //                if ((SParam.OnlyDirect) && (!VFGraph.DirectNeigh(CurrN,i)))continue;
    //                if (!VFGraph.ActiveNeigh(CurrN,i))continue;

    //                size_t NextN=VFGraph.NodeNeigh(CurrN,i);

    //                bool IsTwin=VFGraph.TwinNeigh(CurrN,i);
    //                assert(CurrN!=NextN);

    //                if (!VFGraph.IsActive(NextN))continue;

    //                ScalarType AngleNeigh=VFGraph.AngleNeigh(CurrN,i);
    //                if ((SParam.MaxAngle>0)&&(AngleNeigh>SParam.MaxAngle))continue;

    //                ScalarType LenNeigh=VFGraph.DistNeigh(CurrN,i);

    //                ScalarType NextWeight=CurrWeight+Weight(LenNeigh,AngleNeigh,SParam.DriftPenalty,SParam.MaxAngle);

    //                bool AddHeap=((!VFGraph.IsMarked(NextN))||(VFGraph.Distance(NextN)>NextWeight));
    //                if (SParam.LoopMode)
    //                    AddHeap|=(NextN==SParam.TargetNode);

    //                if (AddHeap)
    //                {
    //                    VFGraph.Mark(NextN);
    //                    VFGraph.Distance(NextN)=NextWeight;
    //                    VFGraph.Father(NextN)=CurrN;
    //                    VFGraph.Jumps(NextN)=CurrJump+1;
    //                    VFGraph.TwinJumps(NextN)=CurrTwin;
    //                    if (IsTwin)VFGraph.TwinJumps(NextN)++;
    //                    Heap.push_back(HeapEntry(NextN,NextWeight));
    //                    push_heap(Heap.begin(),Heap.end());
    //                }
    //            }
    //        }while (!Heap.empty());
    //        return false;
    //    }

    static bool ShortestPath(VertexFieldGraph<MeshType> &VFGraph,
                             const ShortParam& SParam,
                             std::vector<size_t> &PathN,
                             std::vector<ScalarType> *CurrDist=NULL)
    {
        std::vector<HeapEntry> Heap;
        VFGraph.UnMarkAll();

        if (CurrDist!=NULL)
        {
            //std::cout<<"HHHH"<<std::endl;
            assert(SParam.LoopMode==false);
            assert(CurrDist->size()==VFGraph.NumNodes());
            VFGraph.Distances()=(*CurrDist);
            VFGraph.MarkAll();
        }
        if (SParam.LoopMode)
        {
            assert(SParam.StartNode.size()==1);
            //assert(SParam.StartNode[0]==SParam.TargetNode);
        }

        for (size_t i=0;i<SParam.StartNode.size();i++)
        {
            size_t IndexN0=SParam.StartNode[i];
            assert(VFGraph.IsActive(IndexN0));
            VFGraph.Mark(IndexN0);
            VFGraph.Distance(IndexN0)=0;
            VFGraph.Jumps(IndexN0)=0;
            VFGraph.Father(IndexN0)=IndexN0;
            VFGraph.TwinJumps(IndexN0)=0;
            Heap.push_back(HeapEntry(IndexN0,0));
        }

        //set initial
        std::make_heap(Heap.begin(),Heap.end());
        do
        {
            std::pop_heap(Heap.begin(),Heap.end());
            size_t CurrN=(Heap.back()).NodeI;
            ScalarType CurrWeight=VFGraph.Distance(CurrN);//(Heap.back()).Weight;

            //            if ((SParam.OnlyDirect)&&(CurrWeight>0))
            //            {
            //                std::cout<<"**TEST PATH**"<<std::endl;
            //                for (size_t i=0;i<Heap.size();i++)
            //                    std::cout<<Heap[i].NodeI<<" "<<Heap[i].Weight<<std::endl;
            //            }

            //            if ((SParam.OnlyDirect)&&(CurrWeight>0))
            //            {
            //                std::cout<<"Chosen "<<CurrN<< "CurrWeight "<<CurrWeight<<std::endl;
            //            }

            Heap.pop_back();

            //if (!SParam.LoopMode)
            assert(VFGraph.IsMarked(CurrN));


            //then had found the final step
            bool HasTerminated=(SParam.StopAtSel)&&(VFGraph.IsSelected(CurrN));
            HasTerminated|=(SParam.TargetNode==(int)CurrN);


            //check minimal number of jumps (useful for loops)
            int CurrJump=VFGraph.Jumps(CurrN);

            if (HasTerminated)
            {
                //std::cout<<"Retrieving"<<std::endl;
                RetrievePath(VFGraph,CurrN,PathN);
                //std::cout<<"Done"<<std::endl;
                return true;
            }

            size_t CurrTwin=VFGraph.TwinJumps(CurrN);

            if ((SParam.MaxTwin>0)&&((int)CurrTwin>SParam.MaxTwin))continue;

            //std::cout<<"test at "<<CurrJump<<std::endl;
            if ((SParam.MaxJump>0)&&(CurrJump==SParam.MaxJump))continue;//(pow(PropagationSteps,2)+1))continue;//maximum number of jumps reached

            if ((SParam.MaxWeight>0)&&(CurrWeight>SParam.MaxWeight))continue;

            for (size_t i=0;i<VFGraph.NumNeigh(CurrN);i++)
            {
                if ((SParam.OnlyDirect) && (!VFGraph.DirectNeigh(CurrN,i)))continue;
                if (!VFGraph.ActiveNeigh(CurrN,i))continue;

                size_t NextN=VFGraph.NodeNeigh(CurrN,i);

                bool IsTwin=VFGraph.TwinNeigh(CurrN,i);
                assert(CurrN!=NextN);

                bool IsTerminalNext=(SParam.StopAtSel)&&(VFGraph.IsSelected(NextN));
                IsTerminalNext|=(SParam.TargetNode==(int)NextN);
                IsTerminalNext|=(SParam.LoopMode)&&(NextN==SParam.StartNode[0]);

                if ((!IsTwin)&&(SParam.AvoidBorder && VFGraph.IsBorder(NextN))
                        &&(!IsTerminalNext))continue;


                if (!VFGraph.IsActive(NextN))continue;

                ScalarType AngleNeigh=VFGraph.AngleNeigh(CurrN,i);
                if ((SParam.MaxAngle>0)&&(AngleNeigh>SParam.MaxAngle))continue;

                ScalarType LenNeigh=VFGraph.DistNeigh(CurrN,i);

                ScalarType NextWeight=CurrWeight+Weight(LenNeigh,AngleNeigh,SParam.DriftPenalty,SParam.MaxAngle);

                bool IsMarked=(VFGraph.IsMarked(NextN));

                //check if has been explored or not
                ScalarType OldWeight=std::numeric_limits<ScalarType>::max();
                if (IsMarked)
                    OldWeight=VFGraph.Distance(NextN);

                if ((SParam.LoopMode)&&(NextN==SParam.StartNode[0]))
                {
                    RetrievePath(VFGraph,CurrN,PathN);
                    return true;
                }

                if (NextWeight<OldWeight)
                {
                    VFGraph.Mark(NextN);
                    VFGraph.Distance(NextN)=NextWeight;
                    VFGraph.Father(NextN)=CurrN;
                    VFGraph.Jumps(NextN)=CurrJump+1;
                    VFGraph.TwinJumps(NextN)=CurrTwin;

                    if (IsTwin)
                        VFGraph.TwinJumps(NextN)++;

                    Heap.push_back(HeapEntry(NextN,NextWeight));
                    push_heap(Heap.begin(),Heap.end());
                }
            }
        }while (!Heap.empty());
        return false;
    }

    static bool FindLoop(VertexFieldGraph<MeshType> &VFGraph,
                         //ScalarType MaxAngle,ScalarType DriftPenalty,
                         size_t &StartN,std::vector<size_t> &Loop,
                         ScalarType Drift)
    {
        ShortParam SParam;
        SParam.StartNode.push_back(StartN);
        //        SParam.MaxAngle=MaxAngle;
        //        SParam.DriftPenalty=DriftPenalty;
        SParam.MaxJump=-1;
        SParam.MaxTwin=-1;
        SParam.MaxWeight=-1;
        SParam.OnlyDirect=false;
        SParam.StopAtSel=false;
        SParam.TargetNode=-1;
        SParam.LoopMode=true;
        SParam.DriftPenalty=Drift;
        bool Terminated=ShortestPath(VFGraph,SParam,Loop);
        return Terminated;
    }

    static void UpdateDistancesFrom(VertexFieldGraph<MeshType> &VFGraph,
                                    std::vector<size_t> &Sources,
                                    ScalarType Drift,
                                    std::vector<ScalarType> *CurrDist=NULL)
    {
        ShortParam SParam;
        SParam.StartNode=Sources;
        SParam.MaxAngle=0;
        SParam.DriftPenalty=0;
        SParam.MaxJump=-1;
        SParam.MaxTwin=-1;
        SParam.MaxWeight=-1;
        SParam.OnlyDirect=false;
        SParam.StopAtSel=false;
        SParam.TargetNode=-1;
        SParam.DriftPenalty=Drift;
        std::vector<size_t> PathN;
        bool Stopped=ShortestPath(VFGraph,SParam,PathN,CurrDist);
        assert(!Stopped);
    }

    static bool GetSubSequence(VertexFieldGraph<MeshType> &VFGraph,
                               size_t IndexN0,size_t IndexN1,
                               std::vector<size_t > &Sequence,
                               ScalarType Drift)
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

        bool Traced=ShortestPath(VFGraph,SParam,Sequence);
        if (!Traced)return false;
        return true;
    }

    //used to expand a path
    static bool ExpandPath(VertexFieldGraph<MeshType> &VFGraph,
                           std::vector<size_t > &Path,
                           bool IsLoop,
                           ScalarType Drift)
    {
        std::vector<size_t > SwapTraceNode;
        size_t Limit=Path.size()-1;
        if (IsLoop)Limit++;
        for (size_t i=0;i<Limit;i++)
        {
            size_t IndexN0=Path[i];
            size_t IndexN1=Path[(i+1)%Path.size()];
            std::vector<size_t> IndexN;
            bool found=GetSubSequence(VFGraph,IndexN0,IndexN1,IndexN,Drift);
            if(!found)return false;

            assert(IndexN.size()>=2);
            SwapTraceNode.insert(SwapTraceNode.end(),IndexN.begin(),IndexN.end()-1);
        }
        if (!IsLoop)
            SwapTraceNode.push_back(Path.back());

        Path=SwapTraceNode;
        return true;
    }
};

#endif