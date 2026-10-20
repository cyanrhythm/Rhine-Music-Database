const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { checkRelease, publishRelease } = require('../../scripts/publish-release.cjs');
const { createUpdateFeed } = require('../../scripts/create-update-feed.cjs');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'rhine-publish-check-'));
const options = {
    repository: 'MikeQu223/Rhine-Music-Database',
    tag: 'v1.2.3',
    sha: 'a'.repeat(40),
    directory,
};
const releaseId = 42;
const endpoint = `repos/${options.repository}/releases/${releaseId}`;
const calls = [];
let mode = 'success';
let assets = [];

/** 草稿没有可查询的 tag；只有创建返回的 ID 可以读取或公开。 */
function api(method, route, body, file) {
    calls.push({ method, route, body, file });
    assert(!route.includes('/releases/tags/'), 'Draft must not be looked up by tag');
    if (route.includes('/releases?per_page=')) {
        if (mode === 'existing-draft' || mode === 'existing-release') {
            return [
                {
                    tag_name: options.tag,
                    draft: mode === 'existing-draft',
                    html_url: 'https://github.com/draft',
                },
            ];
        }
        return [];
    }
    if (route.includes('/git/matching-refs/')) {
        return mode === 'existing-tag' ? [{ ref: `refs/tags/${options.tag}` }] : [];
    }
    if (method === 'POST' && route === `repos/${options.repository}/releases`) {
        assert.equal(body.tag_name, options.tag);
        assert.equal(body.target_commitish, options.sha);
        assert.equal(body.draft, true);
        return {
            id: releaseId,
            html_url: 'https://github.com/releases/tag/untagged-fixture',
            upload_url:
                mode === 'bad-upload-url'
                    ? 'https://example.com/upload'
                    : `https://uploads.github.com/${endpoint}/assets{?name,label}`,
        };
    }
    if (method === 'POST' && route.startsWith('https://uploads.github.com/')) {
        assert.equal(new URL(route).pathname, `/${endpoint}/assets`);
        if (mode === 'upload-failure') {
            throw new Error('upload failure fixture');
        }
        const name = new URL(route).searchParams.get('name');
        assert.equal(path.basename(file), name);
        const contents = fs.readFileSync(file);
        const asset = {
            name,
            state: 'uploaded',
            size: contents.length,
            digest: 'sha256:' + crypto.createHash('sha256').update(contents).digest('hex'),
        };
        assets.push(asset);
        return asset;
    }
    assert.equal(route, endpoint, 'Read and publish must use the draft ID');
    if (method === 'GET') {
        const uploaded = structuredClone(assets);
        if (mode === 'corrupt') {
            uploaded[0].digest = 'sha256:' + '0'.repeat(64);
        }
        if (mode === 'missing') {
            uploaded.pop();
        }
        return {
            id: releaseId,
            tag_name: options.tag,
            target_commitish: mode === 'identity' ? 'b'.repeat(40) : options.sha,
            draft: true,
            assets: uploaded,
        };
    }
    assert.equal(method, 'PATCH');
    assert.equal(body.draft, false);
    assert.equal(body.make_latest, 'true');
    return { id: releaseId, tag_name: options.tag, draft: false, html_url: 'https://github.com/published' };
}

try {
    for (const platform of ['macOS-arm64.zip', 'Windows-x64.zip', 'Linux-x86_64.AppImage']) {
        fs.writeFileSync(path.join(directory, `RhineMusicDatabase-${options.tag}-${platform}`), platform);
    }
    createUpdateFeed(directory, options.tag, options.repository);
    const result = publishRelease(options, api);
    assert.equal(result.draft, false);
    assert.equal(assets.length, 4);
    assert.equal(calls.filter((call) => call.method === 'PATCH').length, 1);
    for (const failure of ['corrupt', 'missing']) {
        mode = failure;
        assets = [];
        calls.length = 0;
        assert.throws(() => publishRelease(options, api), /mismatch/);
        assert(!calls.some((call) => call.method === 'PATCH'), 'Invalid assets must remain unpublished');
    }
    for (const failure of ['identity', 'upload-failure', 'bad-upload-url']) {
        mode = failure;
        assets = [];
        calls.length = 0;
        assert.throws(() => publishRelease(options, api));
        assert(!calls.some((call) => call.method === 'PATCH'), 'Failures must not publish the draft');
    }
    for (const failure of ['existing-draft', 'existing-tag', 'existing-release']) {
        mode = failure;
        calls.length = 0;
        assert.throws(() => checkRelease(options, api), /draft|Tag already exists|already published/);
        assert(!calls.some((call) => call.method === 'POST'), 'Existing releases must not be modified');
    }
    mode = 'success';
    assert.throws(() => checkRelease({ ...options, tag: 'v1.2.3;echo unsafe' }, api));
    assert.throws(() => checkRelease({ ...options, sha: 'main' }, api));
    calls.length = 0;
    fs.unlinkSync(path.join(directory, 'update.json'));
    assert.throws(() => publishRelease(options, api));
    assert(!calls.some((call) => call.method === 'POST'), 'Missing local packages must not create a draft');
    console.log('Release draft ID, binary uploads, checksums, publish gating and preflight checks passed');
} finally {
    fs.rmSync(directory, { recursive: true, force: true });
}
