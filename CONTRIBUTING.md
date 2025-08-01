# Contributing to Elasticsearch Machine Learning Core

We love to receive contributions from our community — you! There are many ways to contribute, from writing tutorials or blog posts, improving the documentation, submitting bug reports and feature requests or writing code which can be incorporated into the Elastic Stack itself.

We enjoy working with contributors, note we have a [Code of Conduct](https://www.elastic.co/community/codeofconduct), please follow it in all your interactions.

## Bug reports

If you think you have found a bug in ml-cpp, first make sure that you are testing against the latest version of the Elastic Stack - your issue may already have been fixed. Please be aware that the machine learning code is split between the [ml-cpp](https://github.com/elastic/ml-cpp/), [elasticsearch](https://github.com/elastic/elasticsearch) and [kibana](https://github.com/elastic/kibana/) repos. If your bug is related to the UI then the [kibana](https://github.com/elastic/kibana/issues) repo is likely to be a more appropriate place to raise it. And if it's related to the backend REST API then the [elasticsearch](https://github.com/elastic/elasticsearch/issues) repo is likely the most appropriate place.

Before opening a new issue please search the [issues list](https://github.com/elastic/ml-cpp/issues) on GitHub in case a similar issue has already been opened.
It is very helpful if you can prepare a reproduction of the bug. In other words, provide a small test case which we can run to confirm your bug. It makes it easier to find the problem and to fix it.

If you run into a problem that might be a data related issue, something where you can't clearly say it is a bug - for example: "Why is my anomaly not found?" - we encourage you to contact the team via our [Discuss](https://discuss.elastic.co/) forums before opening an issue. 

Important note: It really helps if you provide a data samples that describe any issues found. If you do provide any data samples, then please make sure this data is not in any way confidential. If the data is subject to any license terms, please ensure that there are no restrictions on its usage or redistribution and that attribution is clearly made. 

## Feature requests

If you find yourself wishing for a feature that doesn't exist, you are probably not alone. There are bound to be others out there with similar needs. Open an issue on our issues list on GitHub which describes the feature you would like to see, why you need it, and how it should work.

Similar to bugs, enhancements related to the UI should be raised in the [kibana](https://github.com/elastic/kibana/) repo, and enhancements related to the backend REST API should be raised in the [elasticsearch](https://github.com/elastic/elasticsearch) repo.

## Contributing code and documentation changes

If you have a bugfix or new feature that you would like to contribute to ml-cpp, please find or open an issue about it first. Talk about what you would like to do. It may be that somebody is already working on it, or that there are particular issues that you should know about before implementing the change.
We enjoy working with contributors to get their code accepted. There are many approaches to fixing a problem and it is important to find the best approach before writing too much code.
The process for contributing to any of the [Elastic repositories](https://github.com/elastic/) is similar. Details for individual projects can be found below.
If you want to get started in the project, a good idea is to check issues labeled with help wanted. These are issues that should help newcomers to the project get something achieved without too much hassle.

### Fork and clone the repository

You will need to fork the repository and clone it to your local machine. See the [Github help page](https://help.github.com/articles/fork-a-repo/) for help.

### Submitting your changes

Once your changes and tests are ready to submit for review:
1.  Test your changes

    Run the test suite to make sure that nothing is broken. See below for help running tests.

1.  Sign the Contributor License Agreement

    Please make sure you have signed our [Contributor License Agreement](https://www.elastic.co/contributor-agreement). 
    1.  We are not asking you to assign copyright to us, but to give us the right to distribute your code without restriction. We ask this of all contributors in order to assure our users of the origin and continuing existence of the code. You only need to sign the CLA once.
    1.  If you provide any data samples as part of unit tests or otherwise, then please make sure these do not contain confidential information. If data is subject to any license terms please ensure this attribution is clearly stated. 

1.  Rebase your changes

    Update your local repository with the most recent code from the main ml-cpp repository, and rebase your branch on top of the latest main branch. We prefer your initial changes to be squashed into a single commit. Later, if we ask you to make changes, add them as separate commits. This makes them easier to review. As a final step we will squash all commits when merging your change.

1.  Submit a pull request

    Push your local changes to your forked copy of the repository and [submit a pull request](https://help.github.com/articles/about-pull-requests/). In the pull request, choose a title which sums up the changes that you have made, and in the body provide more details about what your changes do. Also mention the number of the issue where discussion has taken place, eg `Closes #123`. See more information about pull requests [below](#pull-requests)

Then sit back and wait. There will probably be discussion about the pull request and, if any changes are needed, we would love to work with you to get your pull request merged into ml-cpp.

Please adhere to the general guideline that you should never force push to a publicly shared branch. Once you have opened your pull request, you should consider your branch publicly shared. Instead of force pushing you can just add incremental commits; this is generally easier on your reviewers. If you need to pick up changes from main, you can merge main into your branch. A reviewer might ask you to rebase a long-running pull request in which case force pushing is okay for that request. Note that squashing at the end of the review process should also not be done, that can be done when the pull request is [integrated via GitHub](https://blog.github.com/2016-04-01-squash-your-commits/).

## Working with the ml-cpp codebase

**Repository**: https://github.com/elastic/ml-cpp

The build system supports using both `CMake` and `Gradle`. To build, either call `cmake` directly from the top level of the source tree,
for example:

```
cmake -B cmake-build-relwithdebinfo
cmake --build cmake-build-relwithdebinfo -v -j`nproc`
```

Or, using Gradle:

```
./gradlew :compile
```

Note that we configure the build to be of type `RelWithDebInfo` in order to obtain a fully optimized build along with debugging symbols (for Windows native builds this is achieved by adding the flags `--config RelWithDebInfo` to the `cmake --build` command line).

1.  Set up a build machine by following the instructions in the [build-setup](build-setup) directory
1.  Do your changes. 
1.  If you change code, follow the existing [coding style](STYLEGUIDE.md).
    1. You can ensure that the code is formatted correctly by running the `format` target of either `gradle` or `cmake`
        1. gradle: `./gradlew format`
        1. cmake: `cmake --build cmake-build-relwithdebinfo -t format`
1.  Write a test. Unit tests are located under `lib/{module}/unittest` or `bin/{module}/unittest`
1.  The tests are written using the `Boost.Test` framework - https://www.boost.org/doc/libs/1_86_0/libs/test/doc/html/index.html
1.  Test your changes. `cmake --build cmake-build-relwithdebinfo -v -t test` will run _all_ tests in the `ml-cpp` repo.
    1. It also possible to run a subset of tests, or suites of tests, by passing the `TESTS` environment variable to
       `cmake` (wildcards are permitted), e.g. `TESTS="*/testPersist" cmake --build cmake-build-relwithdebinfo -t test_model` would run all tests
       named `testPersist` in all test suites for the `test_model` target.
    1. It is also possible to control the behaviour of the test framework by passing any other arbitrary flags via the
       `TEST_FLAGS` environment variable , e.g. `TEST_FLAGS="--random" cmake --build cmake-build-relwithdebinfo -t test`
       (use TEST_FLAGS="--help" to see the full list).
1. On Linux and maOS it is possible to run individual tests within a Boost Test suite in separate processes.
   1. This is convenient for several reasons:
      1. Isolation: Prevent one test's failures (e.g., memory corruption, unhandled exceptions) from affecting subsequent tests.
      1. Resource Management: Clean up of resources (memory, file handles, network connections) between tests more effectively.
      1. Stability: Improve the robustness of test suites, especially for long-running or complex tests.
      1. Parallelization: A means to run individual test cases in parallel has been provided:
         1. For all tests associated with a library or executable, e.g.
         `cmake --build cmake-build-relwithdebinfo -j 8 -t test_api_individually`
         1. For all tests in the `ml-cpp` repo:
         `cmake --build cmake-build-relwithdebinfo -j 8 -t test_individually`
   1. **Care should be taken that tests don't modify common resources.**
1. As a convenience, there exists a `precommit` target that both formats the code and runs the entire test suite, e.g.
    1. `./gradlew precommit`
    1. `cmake --build cmake-build-relwithdebinfo -j 8 -t precommit`

If you need to test C++ changes in this repo in conjunction with Elasticsearch changes then use
Gradle's `--include-build` option to tell your Elasticsearch build to build the C++ code locally
instead of downloading the latest pre-built bundle. For example, if `elasticsearch` and `ml-cpp`
are cloned in adjacent directories then you could run this in the `elasticsearch` directory:

```
./gradlew :x-pack:plugin:ml:qa:native-multi-node-tests:javaRestTest --tests "org.elasticsearch.xpack.ml.integration.MlJobIT" --include-build ../ml-cpp
```

Before using `--include-build` for the first time when building the C++ together with Elasticsearch
it's necessary to run this command in the `elasticsearch` directory to verify the extra components
used in the C++ build:

```
./gradlew --write-verification-metadata sha256 help --include-build ../ml-cpp
```

## Pull Requests

Every change made to ml-cpp must be held to a high standard, Pull Requests are equally important as they document changes and decisions that have been made. `You Know, for Search` - a descriptive and relevant summary of the change helps people to find your PR later on.

The followings lists some guidelines when authoring pull requests. Note, try to follow this guideline as close as possible but do not feel bad if you are unsure, team members can help you.

1. PR title summarizes the change, short and descriptive.
   1. Use prefixes for quick categorization:
      1. [X.Y] Branch label if backport PR, if omited the PR only applies to main. Note, backport PR's are done after main PR, further explanation can be found below [#backport]
      1. [ML] mandatory prefix, to be consistent with other repositories
      1. [FEATURE] If your pull requests targets a feature branch, this prefix helps as a filter
1. A detailed summary of what changed. Some hints:
    1. Keep it short but do not ommit important details. Usually we squash and merge, write what you would write as commit message of the squashed commit, later you can reuse what you wrote.
    1. A link to each issue that is closed by the PR (e.g. Closes #123) or related (Relates #456)
    1. Further information if necessary, maybe you want to share a screenshot, list open Todo's, describe your thought process, etc.
    1. Optional: List of backport PR's, other related PR's
1. Label the PR, not all might apply:
    1. `:ml` mandatory label, to be consistent with other repositories.
    1. `>type` Type of the PR, e.g. `>bug`, `>refactoring`, `>enhancement`, `>test`.
    1. `vX.Y` Versions that PR should be applied to, a PR on main should always contain all backport versions as well, a backport PR should only have one corresponding version.
    1. `>non-issue` if the PR is not important for the changelog, e.g. a bugfix for an unreleased feature
    1. `affects-results` If the PR is expected to have a significant impact on results(QA test suite), e.g. changes of scoring. Be aware that changes in terms of memory consumption can indirectly lead to significant result changes.
    1. `discuss` If your PR suggests a change for which you'd first like to discuss regarding its functional changes before going deep into actual implementation details (e.g. you change a default)
    1. `WIP` lets potential reviewers know, that you haven't completed the PR yet and you are still working on further changes, lets reviewers know to maybe wait a bit until you finalized the PR.
    1. `review` Try to find reviewers for your change, github has some suggestions, however if unsure or you know the reviewer is very busy, `review` marks the PR as free to grab for review. Still, PR's are open to anyone to comment.

### Backport

Any development usually starts with an implementation on `main`, therefore any PR starts with a PR against `main`. Depending on the type we then decide about backport branches, features usually get backported to the active release branch, e.g. `6.x`, bugfixes might get backported to concrete release branches, e.g. `6.1`. If unsure about the versions to backport to, it can be discussed on the main PR.

A backport starts right after merging the PR on `main`, please open backport PR's immediately after merging to `main`, even if you intend to merge backports a little later (e.g. to wait/analyse QA results). This helps to ensure that backports do not get lost. Rules for Backport PR's:

1. Prefix the title with `[X.Y]` and the mandatory `[ML]`, e.g. `[6.3][ML] Store expanding window bucket values in a compressed format`.
1. Link to the originating PR in the description, you do not need to repeat the description as discussions should exclusively happen on the main PR.
1. Backports do not need a review, however if you had to do further changes (merge conflicts, compiler specialities) it's advised to let the adjustments get quickly reviewed.
1. Label the PR with `>backport` and the corresponding version of this backport, the full set of labels remain on the main PR. If the main PR is classified as `>non-issue` also add the `>non-issue` label due to CI checks.

Although it might look simple, always wait for CI to confirm that changes are sound to avoid unnecessary build breaks.
