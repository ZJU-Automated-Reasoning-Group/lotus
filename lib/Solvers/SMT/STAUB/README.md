# STAUB Readme

from https://github.com/mikekben/STAUB

## Getting Started

STAUB (SMT Theory Arbitrage, from Unbounded to Bounded) speeds up SMT solving of unbounded constraints by converting them to a bounded theory. For ease of use, we provide a pre-built Docker image for testing, and at the end of this file include instructions for a local build, if necessary. We strongly recommend using the Docker image, since building using the dockerfile involves a fresh build of LLVM, which may be time consuming.

To perform simple tests:

+ Run the docker image from https://hub.docker.com/repository/docker/mikekben/staub in interactive mode:
```
docker run -it mikekben/staub
```
Docker should pull the necessary docker image from Dockerhub with the above command, but you may also pull it using `docker pull mikekben/staub`

+ Run STAUB on a sample input using the provided script (this may take up to 1-2 minutes, depending on hardware):
```
./run-sample.sh -int samples/motivating.smt2
-int
samples/motivating.smt2,integer,855,8551462050,23,
Running Z3 on unbounded original ...
sat
 :total-time              27.94)
Running Z3 on transformed bounded constraint ...
sat
 :total-time              1.22)
Running TUNA SMT2LLVM on bounded constraint ...
samples/motivating.smt2,integer,855,8551462050,23,
samples/motivating.smt2-temp.smt2,true,1,1,1,1,1,1,1,1,0.0298969,0.00441312,0.00536189,0,0,0,0,0,0,0,0
Running Z3 on bounded constraint after SMT2LLVM translation ...
sat
 :total-time              0.14)

```
The observed absolute running times may vary across different hardware, but as long as the output matches the form above (and, in particular, the second and third solver runs take substantially less time than the first), STAUB has run correctly, and you have also verified that TUNA's SMT2LLVM tools run correctly on your system.

+ Inspect ``samples/motivating.smt2-bounded.smt2`` to see that the transformation has taken place:
```
cat samples/motivating.smt2-bounded.smt2
```
The contents of the file should match in substance that shown in Figure 1b of the submitted paper (note that several anti-overflow constraints have been omitted in the submitted paper, and a let expression simplified for ease of presentation).

If all three steps listed above execute correctly, then STAUB runs as expected on your system and you should have no technical difficulties with the rest of the artifact.


....
