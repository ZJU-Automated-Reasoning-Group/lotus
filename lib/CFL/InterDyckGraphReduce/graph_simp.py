#!/usr/bin/python3
import argparse
import subprocess
import queue
import itertools
import shutil
import time
import sys

single_directed = True
graphaux = "lotus-cfl-inter-dyck-graphaux"
dkmerge = "lotus-cfl-inter-dyck-dkmerge"
exclude_time = 0.0

# colorreach_mergenode_map: how nodes are merged during one color redeemed as reach
colorreach_mergenode_map = "tmp_color_reach_dk_node_merge.txt"

# the reduced graph after the one color redeemed as reach
colorreach_graph = "tmp_color_reach_dk_graph.dot"

# the node merge result after dk merge for the color reach graph
redmerge_result = "tmp_red_merge_result.txt"

# the edges merges in the reduced graph after dk merge for the color reach graph.
edge_merge_result = "tmp_edge_merge_result.txt"

# the intermediate nodes after dkMerges that indicate edges cannot be removed in the color reach and original graph
dkMerge_interNd_result = "tmp_dkMerge_nodes_representing_cannot_remove_edges.txt"

dotfile = ""

def get_merge_table():
    global exclude_time
    result = dict()
    mergeNodes = dict()
    stime = time.time()
    with open(redmerge_result, "r") as f:
        for line in f:
            leftnum = int(line.split(":")[0])
            rightnum_strs = line.split(": ")[-1].split()
            rightnums = [int(i.strip()) for i in rightnum_strs]
            #print(leftnum, rightnums)
            result[leftnum] = rightnums
    with open(colorreach_mergenode_map, "r") as f:
        for line in f:
            leftnum = int(line.split(":")[0])
            rightnum_strs = line.split(": ")[-1].split()
            rightnums = [int(i.strip()) for i in rightnum_strs]
            #print(leftnum, rightnums)
            mergeNodes[leftnum] = rightnums
    etime = time.time()
    exclude_time += (etime-stime)
    #print("Dk merge map: ")
    #print(result)
    #print("\ncolor reach merge map: ")
    #print(mergeNodes)
    merge_nodes_table = dict()
    for idx, lst in result.items():
        result_set = set()
        for value in lst:
            if value in mergeNodes:
                result_set.update(mergeNodes[value])
            else:
                result_set.add(value)
        #if(len(result_set) > 1):
        #    print(result_set)
        merge_nodes_table[idx] = result_set
    return merge_nodes_table

def get_canceled_edges(merge_node_table, dup_edges):
    '''
    get the used edges in the final reduced graph, plus the parallel edges in dup_edges
    dup_edges is the parallel edges in the color reach graph, which can be used in inter-dyck path
    '''
    global exclude_time
    stime = time.time()
    with open(edge_merge_result, "r") as f:
        edge_strset = set()
        for line in f:
            eid = line.split("eid o")[-1].strip().split("--")[-1].strip()
            afrom = line.split()[1].strip().split("->")[0].strip()
            ato = line.split()[1].strip().split("->")[-1].strip().split(",")[0]
            bfrom = line.split()[3].strip().split("->")[0].strip()
            bto = line.split()[3].strip().split("->")[-1].strip().split(",")[0]

            #print(afrom, ato, bfrom, bto)
            #print(eid)
            asig = "_".join([afrom, ato, eid])
            bsig = "_".join([bfrom, bto, eid])
            edge_strset.add(asig)
            edge_strset.add(bsig)
    
    etime = time.time()
    exclude_time += (etime-stime)
    #print("edge merge result is: ")
    #print(edge_strset)
    edge_strset = edge_strset.union(dup_edges)
    return edge_strset
            
def getEdges(graphfile):
    orig_edge_strset = set()
    stime = time.time()
    with open(graphfile, "r") as f:
        for line in f:
            if "op" not in line:
                continue
            nfrom = line.split("->")[0].strip()
            nto = line.split("[")[0].split("->")[-1].strip()
            eid = line.split("--")[-1].strip().split("\"")[0].strip()
            sig = "_".join([nfrom, nto, eid])
            orig_edge_strset.add(sig)
    etime = time.time()
    exclude_time += (etime-stime)
    return orig_edge_strset

def swap_color(graphfile):
    global exclude_time
    tmp_graphfile = "tmp_" + graphfile
    stime = time.time()
    with open(graphfile, "r") as f:
        with open(tmp_graphfile, "w") as out:
            for line in f:
                if "op--" in line:
                    out.write(line.replace("op--", "ob--"))
                if "ob--" in line:
                    out.write(line.replace("ob--", "op--"))
    shutil.move(tmp_graphfile, graphfile)
    etime = time.time()
    exclude_time += (etime-stime)
    return

def get_reverse_table(mydict):
    '''
    orignal table is unsigned -> {unsigned}, all set disjoint
    '''
    result = dict()
    for key, value in mydict.items():
        for v in value:
            result[v] = key
    return result

def is_inter_edge(nfrom, nto, eid, inter_edges):
    sig = "_".join([str(nfrom), str(nto), str(eid)])
    result = (sig in inter_edges)
    #if result:
        #print(sig, "is an inter edge")
    return result

def remove_edges(used_edges, graphfile, inter_edges, used_merge_nodes_table):
    '''
    according to the used_edges, remove all the unused edges in the graph file, and replace the old 
    graph file with the new graph
    
    inter_nodes is the node id integer set of intermediate nodes

    return whether we successfully remove some edges in the graph
    '''
    edge_remove_flag = False
    reverse_utable = get_reverse_table(used_merge_nodes_table)
    tmp_graphfile = "tmp_" + graphfile
    with open(tmp_graphfile, "w") as out:
        with open(graphfile, "r") as f:
            for line in f:
                if "op" not in line:
                    out.write(line)
                    continue
                #print("\n\nchecking ", line)
                nfrom = int(line.split("->")[0].strip())
                nto = int(line.split("[")[0].split("->")[-1].strip())
                eidstr = line.split("--")[-1].strip().split("\"")[0].strip()
                tmp_nfrom = reverse_utable[nfrom]
                tmp_nto = reverse_utable[nto]
                final_sig = "_".join([str(tmp_nfrom), str(tmp_nto), eidstr])
                if final_sig in used_edges or (is_inter_edge(nfrom, nto, int(eidstr), inter_edges)):
                    out.write(line)
                    #print("yes")
                else:
                    edge_remove_flag = True
    shutil.move(tmp_graphfile, graphfile)
    return edge_remove_flag

def getDupEdges(graphfile):
    nid2mergenid = dict()
    with open(redmerge_result, "r") as f:
        for line in f:
            leftnum = int(line.split(":")[0])
            rightnum_strs = line.split(": ")[-1].split()
            rightnums = [int(i) for i in rightnum_strs]
            for nid in rightnums:
                nid2mergenid[nid] = leftnum
    dups = set()
    edges = set()
    with open(graphfile, "r") as f:
        for line in f:
            if "op" not in line:
                continue
            nfrom = line.split("->")[0].strip()
            nfrom = str(nid2mergenid[int(nfrom)])
            nto = line.split("[")[0].split("->")[-1].strip()
            nfrom = str(nid2mergenid[int(nto)])
            eid = line.split("--")[-1].strip().split("\"")[0].strip()
            sig = "_".join([nfrom, nto, eid])
            if sig in edges:
                dups.add(sig)
            edges.add(sig)
    return dups

def get_inter_nodes(dup_edges):
    '''
    dup_edges has the form of from_to_eid, any dup edges also indicate a intermediate node
    inter_nodes is a set of string of the form "nid_eid", where nid is the intermediate node, eid is the edge id
    that is merged around intermediate node.
    '''
    global exclude_time
    inter_nodes = set()
    stime = time.time()
    with open(dkMerge_interNd_result, "r") as f:
        for line in f:
            nidstr = line.split(":")[0].strip()
            eidstr = line.split("--")[-1].strip()
            sig = "_".join([nidstr, eidstr])
            inter_nodes.add(sig)
    for dupedge in dup_edges:
        tonid = dupedge.split("_")[1]
        eid = dupedge.split("_")[2]
        sig = "_".join([tonid, eid])
        inter_nodes.add(sig)
    etime = time.time()
    exclude_time += (etime-stime)
    return inter_nodes


def clean():
    cmd_make_clean = ["make", "clean"]
    subprocess.call(cmd_make_clean, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cmd_rm = ["rm", "tmp_color_reach_dk_graph.dot", "tmp_color_reach_dk_node_merge.txt", "tmp_dkMerge_nodes_representing_cannot_remove_edges.txt", "tmp_edge_merge_result.txt", "tmp_merge_orig_to_color_merge.txt", "tmp_red_merge_result.txt"]
    subprocess.call(cmd_rm, stderr=subprocess.DEVNULL)

class Node:
    def __init__(self):
        self.in_op_edges = dict()
        self.in_ob_edges = dict()
        self.in_op_edges_eidkey = dict()
        self.in_ob_edges_eidkey = dict()

    def insert_inedge(self, is_op, eid, nid):
        if is_op:
            if nid in self.in_op_edges:
                self.in_op_edges[nid].append(eid)
            else:
                self.in_op_edges[nid] = [eid]
            if eid in self.in_op_edges_eidkey:
                self.in_op_edges_eidkey[eid].append(nid)
            else:
                self.in_op_edges_eidkey[eid] = [nid]
        else:
            if nid in self.in_ob_edges:
                self.in_ob_edges[nid].append(eid)
            else:
                self.in_ob_edges[nid] = [eid]
            if eid in self.in_ob_edges_eidkey:
                self.in_ob_edges_eidkey[eid].append(nid)
            else:
                self.in_ob_edges_eidkey[eid] = [nid]
            



class Graph:
    def __init__(self, graphfile):
        global exclude_time
        self.nodes = dict()
        stime = time.time()
        with open(graphfile, "r") as f:
            for line in f:
                if "cp" in line or "cb" in line:
                    print("Error: has close edge in graph file")
                    exit(1)
                nfrom = line.split("->")[0].strip()
                nto = line.split("[")[0].split("->")[-1].strip()
                eid = line.split("--")[-1].strip().split("\"")[0].strip()
                nfrom = int(nfrom)
                nto = int(nto)
                eid = int(eid)
                is_op = "op" in line
                if nfrom not in self.nodes:
                    self.nodes[nfrom] = Node()
                if nto not in self.nodes:
                    self.nodes[nto] = Node()
                self.nodes[nto].insert_inedge(is_op, eid, nfrom)
        etime = time.time()
        exclude_time += (etime-stime)

def find_inter_edges_bfs(graph, orig_inter_nodes, start, end):
    inter_edges_strset = set()
    used_nodes = set()
    parent = dict()
    worklist = queue.Queue()
    worklist.put(start)
    stop_flag = False
    while not worklist.empty():
        curnid = worklist.get()
        #print("            worklist working on", curnid)
        used_nodes.add(curnid)
        for nextnid, eidlst in graph.nodes[curnid].in_op_edges.items():
            if nextnid not in orig_inter_nodes:
                continue
            if nextnid in used_nodes:
                continue
            #print("            checking", nextnid, "->", curnid)
            used_nodes.add(nextnid)
            parent[nextnid] = (curnid, eidlst[0])
            if(nextnid == end):
                # add the path into inter_edge
                pathnid = nextnid
                while(pathnid != start):
                    tmp_pnid, tmp_eid = parent[pathnid]
                    sig = "_".join([str(pathnid), str(tmp_pnid), str(tmp_eid)])
                    #print("            add inter edge path:", sig)
                    inter_edges_strset.add(sig)
                    pathnid = tmp_pnid
                #stop loop
                stop_flag = True
                break
            worklist.put(nextnid)
        if stop_flag:
            break
    return inter_edges_strset

def find_inter_edges_in_origgraph(graph, orig_inter_nodes, endnodes):
    inter_edges_strset = set()
    for start, end in itertools.combinations(endnodes, 2):
        #print("        Find inter edges between", start, end)
        
        # start <-op- <-op- <-op- end
        #print("            Find", start, "<---", end)
        inter_edges_strset = inter_edges_strset.union(find_inter_edges_bfs(graph, orig_inter_nodes, start, end))
        
        # end <-op- <-op- <-op- start
        #print("            Find", end, "<---", start)
        inter_edges_strset = inter_edges_strset.union(find_inter_edges_bfs(graph, orig_inter_nodes, end, start))
    return inter_edges_strset

def get_inter_edges(inter_nodes, merge_table, graph):
    inter_edges = set()
    global single_directed
    if single_directed is True:
        return inter_edges
    for line in inter_nodes:
        nid = int(line.split("_")[0].strip())
        eid = int(line.split("_")[-1].strip())
        if nid not in merge_table:
            #print(nid, "not in merge_table")
            continue
        orig_nodes = merge_table[nid]
        #print(nid, "orig nodes: ", orig_nodes)
        if(len(orig_nodes) < 2):
            #print(nid, "orig nodes size < 2")
            continue
        endnodes = set()
        for n in orig_nodes:
            # just consider the case that inter edges are op, then eid edges are ob
            if n in graph.nodes and eid in graph.nodes[n].in_ob_edges_eidkey:
                endnodes.add(n)
        #print(nid, "edge id ", eid, " end nodes: ", endnodes)
        if(len(endnodes) < 2):
            #print(nid, "end nodes size < 2")
            continue
        #print("    Finding intermediate edges......")
        inter_edges = inter_edges.union(find_inter_edges_in_origgraph(graph, orig_nodes, endnodes))
    return inter_edges

def iteration():
    '''
    one iteration includes two steps, one for red color "op--", one for blue color "ob--"
    '''
    cmd_graphaux = [graphaux, dotfile]
    cmd_dkmerge = [dkmerge]
    
    # red color reach
    with open(colorreach_graph, "w") as outf:
        subprocess.call(cmd_graphaux, stdout=outf)
    with open(redmerge_result, "w") as redf:
        subprocess.call(cmd_dkmerge, stdout=redf)
    
    merge_nodes_table_first = get_merge_table()
    colorreach_dup_edge_strset_first = getDupEdges(colorreach_graph)
    inter_nodes_first = get_inter_nodes(colorreach_dup_edge_strset_first)
    canceled_edges_first = get_canceled_edges(merge_nodes_table_first, colorreach_dup_edge_strset_first)
    
    #print(inter_nodes_first)
    #print(merge_nodes_table_first)
    
    # change color
    swap_color(dotfile)

    # blue color reach
    with open(colorreach_graph, "w") as outf:
        subprocess.call(cmd_graphaux, stdout=outf)
    with open(redmerge_result, "w") as redf:
        subprocess.call(cmd_dkmerge, stdout=redf)
    
    merge_nodes_table_second = get_merge_table()
    colorreach_dup_edge_strset_second = getDupEdges(colorreach_graph)
    inter_nodes_second = get_inter_nodes(colorreach_dup_edge_strset_second)
    canceled_edges_second = get_canceled_edges(merge_nodes_table_second, colorreach_dup_edge_strset_second)


    # get back to original color, remove blue edges according to the result of canceled_edge_first and inter_edges_second
    swap_color(dotfile)
    orig_graph = Graph(dotfile)
    #print("\n\n")
    #print(inter_nodes_second)
    #print(canceled_edges_first)
    inter_edges_second = get_inter_edges(inter_nodes_second, merge_nodes_table_second, orig_graph)
    #print("Used edges: ", canceled_edges_first, "\nUsed Edges node table: ", merge_nodes_table_first, "\nInter Nodes: ", inter_nodes_second)
    remove_flag = remove_edges(canceled_edges_first, dotfile, inter_edges_second, merge_nodes_table_first)

    # change color 
    swap_color(dotfile)
    orig_graph = Graph(dotfile)
    inter_edges_first = get_inter_edges(inter_nodes_first, merge_nodes_table_first, orig_graph)
    #print("Used edges: ", canceled_edges_second, "\nUsed Edges node table: ", merge_nodes_table_second, "\nInter Nodes: ", inter_edges_first)
    remove_flag = remove_edges(canceled_edges_second, dotfile, inter_edges_first, merge_nodes_table_second) or remove_flag

    # change to original color, finish
    swap_color(dotfile)

    stop_flag = not remove_flag
    return stop_flag


def graph_reduce():
    global single_directed
    clean()
    stop_flag = False
    start_time = time.time()
    while(not stop_flag):
        stop_flag = iteration()
        print("finish one iteration")
    end_time = time.time()
    elapsed_time = end_time - start_time
    print("elapsed time:", elapsed_time-exclude_time)
    #print("Warning: Running in single direction mode?", single_directed)
    clean()
    


# Overview:   Original graph -----------------> one color reach graph
#             one_color_reach_graph -------------------> final_graph
#                                    merge_nodes_table


def main():
    global dotfile, graphaux, dkmerge, single_directed
    parser = argparse.ArgumentParser(
        description="PLDI 2020 interleaved-Dyck graph simplification driver")
    parser.add_argument("input", help="DOT graph to simplify in place")
    parser.add_argument("--graphaux", default=graphaux,
                        help="path to lotus-cfl-inter-dyck-graphaux")
    parser.add_argument("--dkmerge", default=dkmerge,
                        help="path to lotus-cfl-inter-dyck-dkmerge")
    parser.add_argument("--bidirected-input", action="store_true",
                        help="input already represents both directions")
    args = parser.parse_args()
    dotfile = args.input
    graphaux = args.graphaux
    dkmerge = args.dkmerge
    single_directed = not args.bidirected_input
    graph_reduce()


if __name__ == "__main__":
    main()
