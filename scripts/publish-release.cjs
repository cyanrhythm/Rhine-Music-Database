const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');
const { buildVersion } = require('./release-version.cjs');

/** 通过独立参数调用 GitHub CLI */
function githubApi(method, endpoint, body, file) {
    const args = ['api', '--method', method, endpoint];
    const options = { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 };
    if (file) {
        args.push('--header', 'Content-Type: application/octet-stream', '--input', file);
    } else if (body) {
        args.push('--input', '-');
        options.input = JSON.stringify(body);
    }
    const result = spawnSync('gh', args, options);
    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        throw new Error(result.stderr.trim() || `GitHub API failed: ${method} ${endpoint}`);
    }
    return JSON.parse(result.stdout);
}

function validateOptions({ repository, tag, sha }) {
    if (!/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(repository)) {
        throw new Error('Invalid repository');
    }
    if (!/^v/.test(tag) || 'v' + buildVersion(tag).version !== tag) {
        throw new Error('Use a stable release tag such as v1.0.0');
    }
    if (!/^[0-9a-f]{40}$/.test(sha)) {
        throw new Error('Invalid source commit SHA');
    }
}

/** 构建前检查已有正式版、失败草稿及同名 tag，不覆盖任何既有发布内容。 */
function checkRelease(options, api = githubApi) {
    validateOptions(options);
    const { repository, tag } = options;
    for (let page = 1; ; page += 1) {
        const releases = api('GET', `repos/${repository}/releases?per_page=100&page=${page}`);
        const existing = releases.find((release) => release.tag_name === tag);
        if (existing) {
            const message = existing.draft
                ? 'This version already has a draft. Delete the failed draft or choose a new version'
                : 'This version is already published. Choose a new version';
            throw new Error(`${message}: ${existing.html_url}`);
        }
        if (releases.length < 100) {
            break;
        }
    }
    const references = api('GET', `repos/${repository}/git/matching-refs/tags/${tag}`);
    if (references.some((reference) => reference.ref === `refs/tags/${tag}`)) {
        throw new Error('Tag already exists; choose a new version');
    }
}

function checksum(file) {
    const hash = crypto.createHash('sha256');
    const descriptor = fs.openSync(file, 'r');
    const buffer = Buffer.alloc(1024 * 1024);
    try {
        let count;
        while ((count = fs.readSync(descriptor, buffer, 0, buffer.length, null)) > 0) {
            hash.update(buffer.subarray(0, count));
        }
    } finally {
        fs.closeSync(descriptor);
    }
    return 'sha256:' + hash.digest('hex');
}

/**
 * 创建草稿并保存服务端返回的 ID；上传、校验和公开始终使用同一个 ID。
 * 草稿尚未生成正式 tag 时也能访问，任一步失败都保留草稿，不公开不完整产物。
 */
function publishRelease(options, api = githubApi) {
    checkRelease(options, api);
    const { repository, tag, sha, directory } = options;
    const names = [
        `RhineMusicDatabase-${tag}-macOS-arm64.zip`,
        `RhineMusicDatabase-${tag}-Windows-x64.zip`,
        `RhineMusicDatabase-${tag}-Linux-x86_64.AppImage`,
        'update.json',
    ];
    const files = names.map((name) => {
        const file = path.join(directory, name);
        return { name, file, size: fs.statSync(file).size, digest: checksum(file) };
    });
    const draft = api('POST', `repos/${repository}/releases`, {
        tag_name: tag,
        target_commitish: sha,
        name: tag,
        draft: true,
        generate_release_notes: true,
    });
    if (!Number.isSafeInteger(draft.id) || draft.id <= 0) {
        throw new Error('GitHub did not return a valid draft release ID');
    }
    const endpoint = `repos/${repository}/releases/${draft.id}`;
    const upload = new URL(draft.upload_url.split('{')[0]);
    if (
        upload.protocol !== 'https:' ||
        upload.host !== 'uploads.github.com' ||
        upload.username ||
        upload.password ||
        upload.pathname !== `/repos/${repository}/releases/${draft.id}/assets`
    ) {
        throw new Error('Invalid GitHub release upload URL');
    }
    console.log(`Draft release ID: ${draft.id}`);
    for (const file of files) {
        upload.search = new URLSearchParams({ name: file.name }).toString();
        api('POST', upload.toString(), undefined, file.file);
    }
    const release = api('GET', endpoint);
    if (
        release.id !== draft.id ||
        release.tag_name !== tag ||
        release.target_commitish !== sha ||
        !release.draft
    ) {
        throw new Error('Draft release identity changed; refusing to publish');
    }
    if (release.assets.length !== files.length) {
        throw new Error('Release asset count mismatch; draft retained');
    }
    for (const file of files) {
        const matches = release.assets.filter((asset) => asset.name === file.name);
        if (
            matches.length !== 1 ||
            matches[0].state !== 'uploaded' ||
            matches[0].size !== file.size ||
            matches[0].digest !== file.digest
        ) {
            throw new Error(`GitHub asset checksum or size mismatch: ${file.name}; draft retained`);
        }
    }
    const published = api('PATCH', endpoint, {
        tag_name: tag,
        target_commitish: sha,
        draft: false,
        make_latest: 'true',
    });
    if (published.id !== draft.id || published.tag_name !== tag || published.draft !== false) {
        throw new Error('GitHub did not confirm the published release');
    }
    return published;
}

if (require.main === module) {
    try {
        const options = {
            repository: process.env.GITHUB_REPOSITORY,
            tag: process.env.RELEASE_TAG,
            sha: process.env.SOURCE_SHA,
            directory: 'release',
        };
        if (process.argv.includes('--check')) {
            checkRelease(options);
        } else {
            console.log(publishRelease(options).html_url);
        }
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
module.exports = { checkRelease, publishRelease };
