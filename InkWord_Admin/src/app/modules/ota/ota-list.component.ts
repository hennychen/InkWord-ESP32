import { Component, OnInit, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MatTableModule } from '@angular/material/table';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatCardModule } from '@angular/material/card';
import { MatProgressSpinnerModule } from '@angular/material/progress-spinner';
import { MatSnackBar } from '@angular/material/snack-bar';
import { OtaApiService } from '../../core/api/ota-api.service';
import { OtaPackage } from '../../core/models/models';

/**
 * OTA 升级包管理 (A-10/B-17 辅助)：
 * 展示已上传的固件版本列表，支持上传新版本。
 */
@Component({
  selector: 'app-ota-list',
  standalone: true,
  imports: [
    CommonModule,
    MatTableModule,
    MatButtonModule,
    MatIconModule,
    MatCardModule,
    MatProgressSpinnerModule,
  ],
  templateUrl: './ota-list.component.html',
  styleUrl: './ota-list.component.scss',
})
export class OtaListComponent implements OnInit {
  private readonly otaApi = inject(OtaApiService);
  private readonly snackBar = inject(MatSnackBar);

  readonly loading = signal(true);
  readonly uploading = signal(false);
  readonly packages = signal<OtaPackage[]>([]);
  readonly displayedColumns = ['version', 'md5', 'description', 'createdAt', 'downloadUrl'];

  ngOnInit(): void {
    this.loadList();
  }

  private loadList(): void {
    this.loading.set(true);
    this.otaApi.list().subscribe({
      next: (res) => {
        this.packages.set(res.data ?? []);
        this.loading.set(false);
      },
      error: () => this.loading.set(false),
    });
  }

  onUpload(event: Event): void {
    const input = event.target as HTMLInputElement;
    if (!input.files?.length) return;

    const file = input.files[0];
    const version = prompt('请输入固件版本号：');
    if (!version) return;

    this.uploading.set(true);
    this.otaApi.upload(file, version).subscribe({
      next: () => {
        this.snackBar.open('固件上传成功', '关闭', { duration: 2000 });
        this.uploading.set(false);
        this.loadList();
      },
      error: () => this.uploading.set(false),
    });

    // 清空 input 以便重复上传同一文件
    input.value = '';
  }
}
