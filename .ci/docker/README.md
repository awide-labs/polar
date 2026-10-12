# CI images

`Dockerfile.<os>` describes the build and test environment that CI uses for
each distribution in the regression matrix, e.g. `Dockerfile.rocky9`. The
images are published to `ghcr.io/awide-labs/polar/dev/<name>`, e.g.
`ghcr.io/awide-labs/polar/dev/rocky`, for `linux/amd64` and `linux/arm64`.

The images include `pg_bsd_indent` for `src/tools/pgindent`, built from
`src/tools/pg_bsd_indent`, which differs from the upstream version. The
directory is passed to `docker build` as the `pg_bsd_indent` build context,
so build the images with `image.sh` rather than with `docker build` alone.

Each image is tagged `<version>-<hash>`, e.g. `rocky:9-<hash>`, where
`<hash>` is derived from the contents of its Dockerfile and of the
`pg_bsd_indent` sources. A tag therefore always matches one revision of
them, and CI picks the image matching the commit under test:

```shell
.ci/docker/image.sh ref rocky9      # print the image reference
.ci/docker/image.sh ensure rocky9   # pull it, or build it if unpublished
.ci/docker/image.sh build rocky9    # build it
```

## Changing an image

Edit the Dockerfile in a pull request. Its tag is not published yet, so the
CI jobs of the pull request build the image themselves, which adds some
minutes to each job. Once the change is merged into a stable branch, the
`ci-images` workflow publishes the new tag and later runs pull it.

To pick up distribution updates without other changes, make any edit to
the Dockerfile, e.g. to a comment, to get a new tag.

## Local development

The images also work as a development environment. The `<version>` tags,
e.g. `ghcr.io/awide-labs/polar/dev/rocky:9`, point to the latest image
published from the default branch:

```shell
docker run -it --rm -v "$PWD":/home/postgres/polar \
  ghcr.io/awide-labs/polar/dev/rocky:9 bash
```

On other branches, or with a Dockerfile you changed, use the image matching
the checked out sources instead:

```shell
docker run -it --rm -v "$PWD":/home/postgres/polar \
  "$(.ci/docker/image.sh ensure rocky9)" bash
```
